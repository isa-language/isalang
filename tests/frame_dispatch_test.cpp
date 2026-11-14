#include "isac/frames.hpp"
#include "isac/modules.hpp"
#include <filesystem>
#include <fstream>
#include <stdexcept>

int main(int argc, char **argv) {
  using namespace isac;
  using I = core::Inst;
  if (argc != 2)
    return 1;
  auto require = [](bool ok, const char *message) {
    if (!ok)
      throw std::runtime_error(message);
  };
  unsigned rejected = 0;
  for (const char *name : {"owned.isa", "containers.isa"}) {
    std::ifstream input(std::filesystem::path(argv[1]) / name);
    std::string text((std::istreambuf_iterator<char>(input)), {});
    require(!text.empty(), "missing dispatch fixture");
    auto source = core::verify(core::elaborate(resolve(modules::compile(lex(text)).ast)), {true});
    const core::CaptureState *witness = nullptr;
    for (const auto &f : source.get().functions)
      for (const auto &i : f.body)
        if (i.kind == I::Call && source.get().semantics.functions.at(i.function).name == "marker")
          for (const auto &s : source.get().captureStates)
            if (s.function == f.id && s.side == core::StateSide::Before &&
                s.point.block == i.semanticPoint)
              witness = &s;
    require(witness, "missing dispatch witness");
    auto schema = frames::build(source, *witness).get();
    SymbolId selector = None;
    for (const auto &slot : schema.slots)
      if (slot.source.kind == storage::Location::Symbol &&
          source.get().semantics.symbols.at(slot.source.id).name == "stop")
        selector = slot.source.id;
    require(selector != None, "selector is not a semantic capture");
    frames::RoundTripOptions options{{}, true, selector};
    auto make = [&] { return frames::lowerRoundTrip(source, *witness, schema, options); };
    auto body = [&](frames::LoweredRoundTrip &out) -> std::vector<I> & {
      for (auto &f : out.program.functions)
        if (f.id == witness->function)
          return f.body;
      throw std::runtime_error("missing dispatch function");
    };
    auto branch = [&](frames::LoweredRoundTrip &out) -> I & {
      for (auto &i : body(out))
        if (i.kind == I::If && !i.body.empty())
          for (const auto &nested : i.body)
            if (nested.kind == I::Call && nested.function == out.destroy)
              return i;
      throw std::runtime_error("missing generated dispatch");
    };
    auto out = make();
    const auto &fork = branch(out);
    require(fork.body.front().kind == I::Move && fork.body.front().slot == out.heapStorage &&
                fork.body[1].kind == I::Call && fork.body[1].function == out.destroy &&
                fork.body.back().kind == I::Return,
            "destroy edge differs from lifecycle CFG");
    require(fork.otherwise.size() == 3 && fork.otherwise.front().kind == I::Move &&
                fork.otherwise.front().slot == out.heapStorage &&
                fork.otherwise[1].function == out.restore &&
                fork.otherwise.back().kind == I::Bind && fork.otherwise.back().slot == out.storage,
            "restore edge differs from lifecycle CFG");
    ValueId condition = None;
    unsigned selectorReads = 0;
    for (const auto &i : body(out)) {
      if (&i == &fork)
        break;
      if (i.kind == I::Copy && i.slot == selector) {
        condition = i.result;
        ++selectorReads;
      }
    }
    require(selectorReads == 1 && condition != None && fork.args == std::vector{condition},
            "dispatch duplicated or changed selector evaluation");
    auto checked = core::verify(out.program, {true});
    const I *checkedFork = nullptr;
    for (const auto &f : checked.get().functions)
      if (f.id == witness->function)
        for (const auto &i : f.body)
          if (i.kind == I::If)
            for (const auto &nested : i.body)
              if (nested.kind == I::Call && nested.function == out.destroy)
                checkedFork = &i;
    require(checkedFork, "Core lost executable dispatch");
    bool cfgEdges = false;
    for (const auto &g : checked.get().regionGraphs)
      if (g.function == witness->function) {
        const auto &node = g.blocks.at(checkedFork->semanticPoint);
        cfgEdges = node.terminator == regions::Terminator::CondBranch &&
                   node.successors == std::vector{checkedFork->body.front().semanticPoint,
                                                  checkedFork->otherwise.front().semanticPoint} &&
                   g.blocks.at(checkedFork->body.back().semanticPoint).terminator ==
                       regions::Terminator::Return &&
                   g.blocks.at(checkedFork->body.back().semanticPoint).successors.empty();
      }
    require(cfgEdges, "lifecycle edges differ from reverified semantic CFG");
    bool ready = false, restored = false, destroyed = false;
    for (const auto &s : checked.get().captureStates)
      if (s.function == witness->function) {
        if (s.point.block == checkedFork->body.front().semanticPoint &&
            s.side == core::StateSide::Before)
          for (const auto &r : s.resources)
            ready |=
                r.location == storage::Location{storage::Location::Symbol, out.heapStorage, {}} &&
                r.memory == MemoryState::Own;
        if (s.point.block == checkedFork->otherwise.back().semanticPoint &&
            s.side == core::StateSide::After)
          for (const auto &r : s.resources)
            restored |=
                r.location == storage::Location{storage::Location::Symbol, out.storage, {}} &&
                r.memory == MemoryState::Own &&
                r.fields.size() == checked.get().semantics.managedFields(out.frameType).size();
        if (s.point.block == checkedFork->body.back().semanticPoint &&
            s.side == core::StateSide::Before) {
          destroyed = true;
          for (const auto &r : s.resources)
            require(!checked.get().semantics.linear(r.type) || r.memory == MemoryState::Dead,
                    "destroy return retained an owned resource");
        }
      }
    if (!(ready && restored && destroyed))
      throw std::runtime_error(
          "state/storage correspondence missing: ready=" + std::to_string(ready) +
          " restored=" + std::to_string(restored) + " destroyed=" + std::to_string(destroyed));
    auto reject = [&](auto change) {
      auto corrupt = make();
      change(corrupt);
      try {
        (void)core::verify(std::move(corrupt.program));
        throw std::runtime_error("unsafe dispatch Core accepted");
      } catch (const Error &) {
        ++rejected;
      }
    };
    reject([&](auto &p) { branch(p).body.pop_back(); });
    reject([&](auto &p) { branch(p).body[1].function = p.restore; });
    reject([&](auto &p) {
      auto &b = branch(p).body;
      auto twice = b[0];
      twice.result = p.program.values++;
      b.insert(b.begin() + 1, twice);
    });
    reject([&](auto &p) { branch(p).otherwise.pop_back(); });
    reject([&](auto &p) {
      auto &b = branch(p);
      b.body[1].args = {b.otherwise.front().result};
    });
    reject([&](auto &p) {
      auto &b = branch(p);
      b.args = {b.body.front().result};
    });
    reject([&](auto &p) {
      auto &b = branch(p).body;
      b.insert(b.begin() + 2, b[1]);
    });
    reject([&](auto &p) {
      auto &b = branch(p);
      b.otherwise[1].args = {b.body.front().result};
    });
    SymbolId managed = None;
    for (const auto &slot : schema.slots)
      if (slot.mode == core::CaptureMode::Move && slot.source.kind == storage::Location::Symbol)
        managed = slot.source.id;
    require(managed != None, "missing owned dispatch input");
    for (auto invalid : std::vector<frames::RoundTripOptions>{
             {{}, false, selector}, {{0}, true, selector}, {{}, true, None}, {{}, true, managed}}) {
      try {
        (void)frames::lowerRoundTrip(source, *witness, schema, invalid);
        throw std::runtime_error("unsupported dispatch input accepted");
      } catch (const Error &e) {
        require(e.code == "E8006", "wrong dispatch gate diagnostic");
      }
    }
    (void)frames::roundTripEntry(source, {}, {true});
    (void)frames::roundTripEntry(source, {{}, true}, {true}, "stop");
  }
  require(rejected == 16, "missing malformed dispatch coverage");
}
