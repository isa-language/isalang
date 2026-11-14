#include "isac/backend.hpp"
#include "isac/erasure.hpp"
#include "isac/frames.hpp"
#include "isac/modules.hpp"
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>

int main(int argc, char **argv) {
  using namespace isac;
  using I = core::Inst;
  if (argc != 2 && argc != 6)
    return 1;
  auto require = [](bool ok, const char *message) {
    if (!ok)
      throw std::runtime_error(message);
  };
  const auto target = selectTarget({});
  auto load = [&](const char *path) {
    std::ifstream input(std::filesystem::path(argv[1]) / path);
    std::string text((std::istreambuf_iterator<char>(input)), {});
    require(!text.empty(), "missing outline fixture");
    return core::verify(core::elaborate(resolve(modules::compile(lex(text)).ast, target)), {true});
  };
  auto marker = [&](const core::Program &p) -> const core::CaptureState & {
    for (const auto &f : p.functions)
      for (const auto &i : f.body)
        if (i.kind == I::Call && p.semantics.functions.at(i.function).name == "marker")
          for (const auto &s : p.captureStates)
            if (s.function == f.id && s.point.block == i.semanticPoint &&
                s.side == core::StateSide::Before)
              return s;
    throw std::runtime_error("missing outline boundary");
  };
  auto function = [&](core::Program &p, FunctionId id) -> core::Function & {
    for (auto &f : p.functions)
      if (f.id == id)
        return f;
    throw std::runtime_error("missing outlined function");
  };

  if (argc == 6) {
    require(std::string(argv[2]) == "--build-owned-result", "unknown outline fixture mode");
    auto source = load("frame_outline/owned_result.isa");
    const auto &point = marker(source.get());
    auto checked =
        frames::roundTrip(source, point, frames::build(source, point).get(), {{}, true, {}, true});
    NativeOptions options;
    options.target = target;
    options.counters = true;
    options.optimization =
        std::string(argv[4]) == "O2" ? OptimizationLevel::O2 : OptimizationLevel::O0;
    options.sanitize = std::string(argv[5]) == "sanitized";
    auto ir = generate(runtime::lower(erase(checked)), BackendStage::LLVMIR, options.sanitize);
    compileNative(optimizeLLVM(ir, target, options.optimization), argv[3], options);
    return 0;
  }
  unsigned rejected = 0;
  for (const char *path : {"frame_heap/valid_owned.isa", "frame_heap/valid_nested.isa",
                           "frame_heap/valid_containers.isa", "frame_heap/valid_copy.isa",
                           "frame_heap/valid_regions.isa", "frame_heap/valid_branches.isa",
                           "frame_outline/locals.isa", "frame_outline/owned_result.isa"}) {
    auto source = load(path);
    const auto &point = marker(source.get());
    auto schema = frames::build(source, point).get();
    auto make = [&] { return frames::lowerRoundTrip(source, point, schema, {{}, true, {}, true}); };
    auto out = make();
    require(out.continuation != None, "continuation was not extracted");
    const auto &signature = out.program.semantics.functions.at(out.continuation);
    require(!signature.trusted && signature.intrinsicId == IntrinsicId::None &&
                signature.owner == None && signature.kind == FunctionKind::Normal &&
                signature.params.size() == 1 &&
                out.program.semantics.symbols.at(signature.params[0]).type == out.heapType &&
                signature.result == source.get().semantics.functions.at(point.function).result &&
                signature.parameterRegions.empty(),
            "continuation uses an unchecked calling convention");
    SymbolId previous = 0;
    for (const auto &[old, fresh] : out.continuationSymbols) {
      require(fresh != old && fresh > previous,
              "alpha-renaming changed relative destruction order");
      previous = fresh;
      const auto &before = source.get().semantics.symbols;
      const auto &after = out.program.semantics.symbols.at(fresh);
      require(after.parameter == (old == out.heapStorage),
              "source parameter escaped the single-frame calling convention");
      if (old < before.size()) {
        require(after.type == before[old].type && after.name == before[old].name,
                "cloning lost semantic type/origin");
        if (before[old].region != None)
          require(after.region != before[old].region && after.region != None,
                  "outlined local reused a source region");
      }
    }
    auto checked = core::verify(out.program, {true});
    (void)erase(checked);
    bool ownedParameter = false;
    for (const auto &s : checked.get().captureStates)
      if (s.function == out.continuation)
        for (const auto &r : s.resources)
          ownedParameter |=
              r.location == storage::Location{storage::Location::Symbol, signature.params[0], {}} &&
              r.memory == MemoryState::Own && !r.callerOwned;
    require(ownedParameter, "continuation input is not verified owned storage");
    auto reject = [&](auto corrupt) {
      auto altered = make();
      corrupt(altered);
      try {
        (void)core::verify(std::move(altered.program));
        throw std::runtime_error("invalid outlined Core accepted");
      } catch (const Error &) {
        ++rejected;
      }
    };
    reject(
        [&](auto &p) { p.program.semantics.functions[p.continuation].kind = FunctionKind::Async; });
    reject([&](auto &p) {
      auto &b = function(p.program, p.continuation).body;
      auto duplicate = b.front();
      duplicate.result = p.program.values++;
      b.insert(b.begin() + 1, duplicate);
    });
    reject([&](auto &p) {
      auto &b = function(p.program, p.continuation).body;
      b.front().slot = p.heapStorage;
    });
    reject([&](auto &p) {
      auto &b = function(p.program, p.continuation).body;
      for (auto &i : b)
        if (i.kind == I::Call && i.function == p.restore) {
          i.function = p.destroy;
          return;
        }
      throw std::runtime_error("missing restore");
    });
    reject([&](auto &p) {
      auto &b = function(p.program, point.function).body;
      for (size_t n = 0; n < b.size(); ++n)
        if (b[n].kind == I::Call && b[n].function == p.continuation) {
          auto duplicate = b[n];
          if (duplicate.result != None)
            duplicate.result = p.program.values++;
          b.insert(b.begin() + n + 1, duplicate);
          return;
        }
      throw std::runtime_error("missing continuation call");
    });
    reject([&](auto &p) {
      auto &b = function(p.program, p.continuation).body;
      for (auto &i : b)
        if (i.kind == I::FieldTake || i.kind == I::Read) {
          i.slot = p.storage;
          return;
        }
      throw std::runtime_error("missing field restore");
    });
    for (auto invalid :
         std::vector<frames::RoundTripOptions>{{{}, false, {}, true}, {{0}, true, {}, true}}) {
      try {
        (void)frames::lowerRoundTrip(source, point, schema, invalid);
        throw std::runtime_error("unsupported outline accepted");
      } catch (const Error &e) {
        require(e.code == "E8006", "incorrect outline gate diagnostic");
      }
    }
  }
  require(rejected == 48, "missing outlined Core corruption coverage");

  for (const char *path : {"frame_heap/blocked_protocol.isa", "frame_heap/blocked_borrow.isa",
                           "frame_heap/blocked_partial.isa"}) {
    auto source = load(path);
    const auto &point = marker(source.get());
    try {
      (void)frames::lowerRoundTrip(source, point, frames::build(source, point).get(),
                                   {{}, true, {}, true});
      throw std::runtime_error("unsupported capture crossed the outlined function boundary");
    } catch (const Error &e) {
      require(e.code == "E8006", "incorrect capture gate diagnostic");
    }
  }
  auto empty = load("frame_outline/empty.isa");
  bool emptyChecked = false;
  for (const auto &s : empty.get().captureStates)
    if (s.cleanupValid && !s.terminal && s.resources.empty()) {
      auto out =
          frames::lowerRoundTrip(empty, s, frames::build(empty, s).get(), {{}, true, {}, true});
      require(out.continuation == None && out.heapType == None,
              "empty capture allocated a fictitious frame");
      (void)core::verify(std::move(out.program));
      emptyChecked = true;
      break;
    }
  require(emptyChecked, "missing empty-frame outline coverage");
}
