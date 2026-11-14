#include "isac/frames.hpp"
#include "isac/erasure.hpp"
#include "isac/modules.hpp"
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>

int main(int argc, char **argv) {
  using namespace isac;
  if (argc != 2)
    return 1;
  auto require = [](bool ok, const char *text) {
    if (!ok)
      throw std::runtime_error(text);
  };
  auto load = [&](const char *name) {
    std::ifstream input(std::filesystem::path(argv[1]) / name);
    std::string text((std::istreambuf_iterator<char>(input)), {});
    require(!text.empty(), "missing frame storage fixture");
    return core::verify(core::elaborate(resolve(modules::compile(lex(text)).ast)), {true});
  };
  auto atMarker = [&](const auto &p) -> const core::CaptureState & {
    auto main = p.semantics.entryPoint.function;
    for (const auto &f : p.functions)
      if (f.id == main)
        for (const auto &i : f.body)
          if (i.kind == core::Inst::Call && p.semantics.functions[i.function].name == "marker")
            for (const auto &s : p.captureStates)
              if (s.function == main && s.point.block == i.semanticPoint &&
                  s.side == core::StateSide::Before)
                return s;
    throw std::runtime_error("no marker boundary");
  };
  auto source = load("valid_owned.isa");
  const auto &witness = atMarker(source.get());
  auto schema = frames::build(source, witness).get();
  auto lower = frames::lowerRoundTrip(source, witness, schema);
  std::set<ValueId> definitions;
  std::function<void(const std::vector<core::Inst> &)> collect = [&](const auto &body) {
    for (const auto &i : body) {
      if (i.result != None)
        require(definitions.insert(i.result).second, "duplicate transformed SSA definition");
      collect(i.condition);
      collect(i.body);
      collect(i.otherwise);
      collect(i.latch);
    }
  };
  for (const auto &f : lower.program.functions) {
    for (auto id : f.parameters)
      require(definitions.insert(id).second, "duplicate transformed parameter");
    collect(f.body);
  }
  require(definitions.size() == lower.program.values,
          "transformed SSA retained sparse version-only IDs");
  for (ValueId id = 0; id < lower.program.values; ++id)
    require(definitions.contains(id), "transformed SSA lost density");
  require(lower.frameType != None && lower.storage != None && lower.restoredSymbols.size() == 2,
          "owned sources not materialized");
  const auto &shape = lower.program.semantics.types.at(lower.frameType);
  require(shape.representation == hir::Representation::Stack &&
              shape.fields.size() == schema.slots.size(),
          "frame layout is not an ordinary stack aggregate");
  for (auto [old, restored] : lower.restoredSymbols)
    require(old == restored, "restoration reordered resource identity/destruction");
  auto checked = core::verify(lower.program, {true});
  bool inFrame = false, restored = false;
  for (const auto &s : checked.get().captureStates) {
    if (s.function != witness.function)
      continue;
    bool owner = false, storage = false, partial = false;
    for (const auto &r : s.resources)
      if (r.location.kind == storage::Location::Symbol) {
        owner |= lower.restoredSymbols.contains(r.location.id);
        if (r.location.id == lower.storage) {
          storage = true;
          partial |= !r.absentFields.empty();
        }
      }
    inFrame |= storage && !owner;
    restored |= owner && storage && partial;
  }
  require(inFrame && restored, "resource checker did not observe transfer/restoration states");
  auto make = [&] { return frames::lowerRoundTrip(source, witness, schema); };
  unsigned corruptions = 0;
  auto reject = [&](auto corrupt) {
    auto changed = make();
    corrupt(changed);
    try {
      (void)core::verify(std::move(changed.program));
      throw std::runtime_error("invalid transformed continuation accepted");
    } catch (const Error &) {
      ++corruptions;
    }
  };
  auto mainBody = [&](auto &p) -> std::vector<core::Inst> & {
    for (auto &f : p.functions)
      if (f.id == witness.function)
        return f.body;
    throw std::runtime_error("missing transformed function");
  };
  reject([&](auto &p) {
    auto &body = mainBody(p.program);
    for (size_t n = 0; n < body.size(); ++n)
      if (body[n].kind == core::Inst::FieldTake && body[n].slot == p.storage) {
        body.insert(body.begin() + n, body[n]);
        return;
      }
  });
  reject([&](auto &p) {
    auto &body = mainBody(p.program);
    for (auto &i : body)
      if (i.kind == core::Inst::FieldStore && i.slot == p.storage) {
        i.fieldIndex = 999;
        return;
      }
  });
  reject([&](auto &p) {
    auto &body = mainBody(p.program);
    for (auto &i : body)
      if (i.kind == core::Inst::FieldTake && i.slot == p.storage) {
        i.type = Bool;
        return;
      }
  });
  reject([&](auto &p) {
    auto &body = mainBody(p.program);
    for (size_t n = 0; n < body.size(); ++n)
      if (body[n].kind == core::Inst::FieldTake && body[n].slot == p.storage) {
        core::Inst use{};
        use.kind = core::Inst::Move;
        use.type = schema.slots[0].type;
        use.quantity = Quantity::One;
        use.slot = schema.slots[0].source.id;
        use.result = p.program.values++;
        body.insert(body.begin() + n, use);
        return;
      }
  });
  reject([&](auto &p) {
    auto &body = mainBody(p.program);
    for (auto &i : body)
      if (i.kind == core::Inst::Bind && p.restoredSymbols.contains(i.slot)) {
        i.slot = p.storage;
        return;
      }
  });
  reject([&](auto &p) {
    auto &body = mainBody(p.program);
    for (auto it = body.begin(); it != body.end(); ++it)
      if (it->kind == core::Inst::FieldStore && it->slot == p.storage) {
        body.erase(it);
        return;
      }
  });

  reject([&](auto &p) {
    auto &body = mainBody(p.program);
    for (size_t n = 0; n + 1 < body.size(); ++n)
      if (body[n].kind == core::Inst::FieldTake && body[n].slot == p.storage) {
        auto value = body[n];
        auto bind = body[n + 1];
        value.fieldIndex = 1 - value.fieldIndex;
        value.result = p.program.values++;
        bind.args = {value.result};
        body.insert(body.begin() + n + 2, {value, bind});
        return;
      }
  });
  require(corruptions == 7, "missing transformed Core corruptions");

  for (uint32_t prefix = 0; prefix <= schema.slots.size(); ++prefix) {
    auto exit = frames::lowerRoundTrip(source, witness, schema, {{prefix}});
    (void)core::verify(std::move(exit.program));
  }
  try {
    (void)frames::lowerRoundTrip(source, witness, schema, {{999}});
    throw std::runtime_error("invalid construction prefix accepted");
  } catch (const Error &e) {
    require(e.code == "E8006", "wrong construction prefix error");
  }

  for (const char *name : {"valid_containers.isa", "valid_nested.isa", "valid_branches.isa",
                           "valid_copy.isa", "valid_regions.isa"}) {
    auto p = load(name);
    const auto &s = atMarker(p.get());
    auto frame = frames::build(p, s);
    (void)frames::roundTrip(p, s, frame.get(), {}, {true});
  }

  auto copy = load("valid_copy.isa");
  const auto &cs = atMarker(copy.get());
  auto scalar = frames::lowerRoundTrip(copy, cs, frames::build(copy, cs).get());
  bool edited = false;
  for (auto &f : scalar.program.functions)
    for (auto &i : f.body)
      if (i.kind == core::Inst::Read && i.slot == scalar.storage &&
          i.placeProjection == core::Inst::PlaceProjection::Field) {
        i.fieldIndex = 999;
        edited = true;
        break;
      }
  require(edited, "missing typed scalar projection");
  try {
    (void)core::verify(std::move(scalar.program));
    throw std::runtime_error("invalid scalar projection accepted");
  } catch (const Error &e) {
    require(e.code == "E9001", "wrong scalar projection diagnostic");
  }

  auto empty = core::verify(
      core::elaborate(resolve(modules::compile(lex("fn main(){print(7);}")).ast)), {true});
  bool emptyExit = false;
  for (const auto &s : empty.get().captureStates)
    if (s.function == empty.get().semantics.entryPoint.function &&
        s.side == core::StateSide::Before && s.cleanupValid && s.resources.empty()) {
      auto out = frames::lowerRoundTrip(empty, s, frames::build(empty, s).get(), {{0}});
      require(out.storage == None && out.frameType == None,
              "empty capture allocated artificial storage");
      for (const auto &f : out.program.functions)
        if (f.id == s.function)
          require(f.body.size() == 1 && f.body[0].kind == core::Inst::Return,
                  "empty construction exit resumed its tail");
      (void)core::verify(std::move(out.program));
      emptyExit = true;
      break;
    }
  require(emptyExit, "missing empty construction-exit coverage");
}
