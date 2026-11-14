#include "isac/frames.hpp"
#include "isac/frame_lifecycle.hpp"
#include "isac/erasure.hpp"
#include "isac/modules.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

int main(int argc, char **argv) {
  using namespace isac;
  if (argc != 2)
    return 1;
  auto require = [](bool ok, const char *message) {
    if (!ok)
      throw std::runtime_error(message);
  };
  auto load = [&](const char *name) {
    std::ifstream input(std::filesystem::path(argv[1]) / name);
    std::string text((std::istreambuf_iterator<char>(input)), {});
    require(!text.empty(), "missing heap frame fixture");
    return core::verify(core::elaborate(resolve(modules::compile(lex(text)).ast)), {true});
  };
  auto marker = [&](const core::Program &p) -> const core::CaptureState & {
    for (const auto &f : p.functions)
      for (const auto &i : f.body)
        if (i.kind == core::Inst::Call && p.semantics.functions[i.function].name == "marker")
          for (const auto &s : p.captureStates)
            if (s.function == f.id && s.point.block == i.semanticPoint &&
                s.side == core::StateSide::Before)
              return s;
    throw std::runtime_error("missing marker witness");
  };
  auto function = [&](core::Program &p, FunctionId id) -> core::Function & {
    for (auto &f : p.functions)
      if (f.id == id)
        return f;
    throw std::runtime_error("missing generated function");
  };
  auto source = load("frame_heap/valid_owned.isa");
  const auto &witness = marker(source.get());
  auto schema = frames::build(source, witness).get();
  using O = frames::LifecycleOp;
  using S = frames::ContinuationState;
  auto construction = frames::build(source, witness);
  auto resumed = frames::buildLifecycle(construction, false);
  require(resumed.trace() == std::vector{S::Ready, S::Restored, S::Delivered, S::Delivered},
          "incorrect continuation trace");
  auto destroyed = frames::buildLifecycle(construction, true);
  require(destroyed.trace() == std::vector{S::Ready, S::Destroyed, S::Destroyed},
          "incorrect destruction trace");
  std::ostringstream lifecycleDump;
  frames::print(resumed, lifecycleDump);
  frames::print(destroyed, lifecycleDump);
  require(lifecycleDump.str().find("ready -> restore -> deliver -> finish") != std::string::npos &&
              lifecycleDump.str().find("ready -> destroy -> finish") != std::string::npos,
          "missing lifecycle dump");
  for (auto invalid : std::vector<std::vector<O>>{{},
                                                  {O::Finish},
                                                  {O::Restore, O::Finish},
                                                  {O::Deliver, O::Finish},
                                                  {O::Restore, O::Restore, O::Deliver, O::Finish},
                                                  {O::Destroy, O::Destroy, O::Finish},
                                                  {O::Restore, O::Destroy, O::Finish},
                                                  {O::Destroy, O::Restore, O::Finish},
                                                  {O::Restore, O::Deliver, O::Deliver, O::Finish},
                                                  {O::Destroy, O::Finish, O::Restore},
                                                  {O::Restore, O::Deliver},
                                                  {static_cast<O>(999), O::Finish}}) {
    try {
      (void)frames::verifyLifecycle(construction, invalid);
      throw std::runtime_error("invalid lifecycle accepted");
    } catch (const Error &e) {
      require(e.code == "E9001", "wrong lifecycle error");
    }
  }

  using G = frames::LifecycleGraph;
  auto graph = [&](G proposal) {
    return frames::verifyLifecycleGraph(construction, std::move(proposal));
  };
  auto split = graph(
      {0,
       {{{}, {1, 2}}, {{O::Restore, O::Deliver, O::Finish}, {}}, {{O::Destroy, O::Finish}, {}}}});
  require(split.blockFacts()[1].incoming == std::set{S::Ready} &&
              split.blockFacts()[2].outgoing == std::set{S::Destroyed},
          "branch ownership facts lost");
  auto joined = graph(
      {0, {{{}, {1, 2}}, {{O::Restore}, {3}}, {{O::Restore}, {3}}, {{O::Deliver, O::Finish}, {}}}});
  require(joined.blockFacts()[3].incoming == std::set{S::Restored}, "restored join is not precise");

  auto finished = graph(
      {0, {{{}, {1, 2}}, {{O::Restore, O::Deliver}, {3}}, {{O::Destroy}, {3}}, {{O::Finish}, {}}}});
  require(finished.blockFacts()[3].incoming == std::set{S::Delivered, S::Destroyed} &&
              finished.blockFacts()[3].outgoing == std::set{S::Delivered, S::Destroyed},
          "join discarded an alternative ownership state");
  auto loop = graph({0, {{{}, {0, 1}}, {{O::Destroy, O::Finish}, {}}}});
  require(loop.blockFacts()[0].incoming == std::set{S::Ready},
          "ownership-preserving backedge failed to stabilize");

  auto reorderedGraph =
      graph({2, {{{}, {1}}, {{}, {0, 3}}, {{}, {0}}, {{O::Restore, O::Deliver, O::Finish}, {}}}});
  require(reorderedGraph.blockFacts()[1].outgoing == std::set{S::Ready} &&
              reorderedGraph.construction().origin().function == construction.origin().function,
          "CFG lost entry identity or source construction proof");
  std::ostringstream graphDump;
  frames::print(finished, graphDump);
  require(graphDump.str().find(
              "bb3 in={ delivered destroyed } out={ delivered destroyed } successors={ }") !=
                  std::string::npos &&
              graphDump.str().find("successors={ bb1 bb2 }") != std::string::npos,
          "missing lifecycle CFG facts in dump");
  auto rejectGraph = [&](G proposal, const char *reason) {
    try {
      (void)graph(std::move(proposal));
      throw std::runtime_error("invalid lifecycle CFG accepted");
    } catch (const Error &e) {
      require(e.code == "E9001" && std::string(e.what()).find(reason) != std::string::npos,
              "lifecycle CFG rejected for the wrong reason");
    }
  };
  rejectGraph({}, "valid entry");
  rejectGraph({1, {{{O::Destroy, O::Finish}, {}}}}, "valid entry");
  rejectGraph({0, {{{}, {1}}}}, "successor");
  rejectGraph({0, {{{}, {1, 1}}, {{O::Destroy, O::Finish}, {}}}}, "successor");
  rejectGraph({0, {{{O::Destroy, O::Finish}, {}}, {{O::Destroy, O::Finish}, {}}}},
              "unreachable block");
  rejectGraph({0, {{{O::Restore, O::Deliver}, {}}}}, "exit lacks Finish");
  rejectGraph({0, {{{O::Destroy, O::Finish}, {0}}}}, "continues after Finish");
  rejectGraph({0, {{{O::Destroy, O::Finish, O::Restore}, {}}}}, "continues after Finish");
  rejectGraph({0, {{{}, {0}}}}, "no path to Finish");
  rejectGraph({0, {{{}, {1, 2}}, {{}, {1}}, {{O::Destroy, O::Finish}, {}}}}, "no path to Finish");
  rejectGraph({0, {{{}, {1, 2}}, {{O::Restore}, {3}}, {{}, {3}}, {{O::Deliver, O::Finish}, {}}}},
              "delivery requires");
  rejectGraph({0,
               {{{}, {1, 2}},
                {{O::Restore, O::Deliver}, {3}},
                {{O::Destroy}, {3}},
                {{O::Deliver, O::Finish}, {}}}},
              "delivery requires");
  rejectGraph({0, {{{O::Restore}, {0, 1}}, {{O::Deliver, O::Finish}, {}}}}, "restoration requires");
  rejectGraph({0, {{{O::Destroy}, {0, 1}}, {{O::Finish}, {}}}}, "destruction requires");

  rejectGraph({0, {{{O::Restore}, {1}}, {{}, {0, 2}}, {{O::Deliver, O::Finish}, {}}}},
              "restoration requires");
  rejectGraph({0, {{{}, {1, 2}}, {{O::Restore}, {3}}, {{O::Destroy}, {3}}, {{O::Finish}, {}}}},
              "unconsumed");
  rejectGraph({0, {{{static_cast<O>(999), O::Destroy, O::Finish}, {}}}}, "unknown");
  auto located = G{0, {{{O::Restore, O::Finish}, {}, {}}}};
  located.blocks[0].origin = witness.origin;
  try {
    (void)graph(located);
    throw std::runtime_error("abandoned restored aggregate accepted");
  } catch (const Error &e) {
    require(e.loc.span.file == located.blocks[0].origin.span.file &&
                e.loc.span.start == located.blocks[0].origin.span.start,
            "lifecycle failure lost the producer's source origin");
  }
  auto make = [&] { return frames::lowerRoundTrip(source, witness, schema, {{}, true}); };
  auto lowered = make();
  auto &p = lowered.program;
  const auto &types = p.semantics;
  require(lowered.heapType != None && lowered.heapStorage != None, "no real heap owner");
  require(types.types.at(lowered.heapType).representation == hir::Representation::GenericBox &&
              types.types.at(lowered.heapType).element == lowered.frameType,
          "heap frame bypasses normal Box representation");
  bool nominal = false;
  for (const auto &instance : types.instances)
    if (instance.type == lowered.heapType) {
      require(instance.declaration >= types.genericEnums.size(), "Box instance uses enum identity");
      const auto &decl = types.genericStructs.at(instance.declaration - types.genericEnums.size());
      require(decl.representation == hir::Representation::GenericBox &&
                  instance.substitution.types.size() == 1 &&
                  instance.substitution.types[0] ==
                      std::pair{decl.parameters[0], lowered.frameType},
              "Box instance lost generic substitution");
      auto size = p.semantics.instances.size();
      require(hir::instantiateHeapContainer(p.semantics, instance.declaration, {lowered.frameType},
                                            {}) == lowered.heapType &&
                  p.semantics.instances.size() == size,
              "shared generic instantiation cache duplicated the type");
      try {
        (void)hir::instantiateHeapContainer(p.semantics, instance.declaration, {RawPtr}, {});
        throw std::runtime_error(
            "container specialization admitted unsafe/reference-bearing storage");
      } catch (const Error &) {
      }
      nominal = true;
      break;
    }
  require(nominal, "heap frame has no nominal GenericInstance");
  for (auto id : {lowered.factory, lowered.restore, lowered.destroy}) {
    const auto &f = p.semantics.functions.at(id);
    require(f.owner == None && !f.trusted && f.intrinsicId == IntrinsicId::None &&
                f.kind == FunctionKind::Normal && f.params.size() == 1,
            "frame helper is an unchecked intrinsic");
    require(!function(p, id).body.empty(), "frame helper has no Core body");
  }
  require(types.functions.at(lowered.factory).result == lowered.heapType &&
              types.functions.at(lowered.restore).result == lowered.frameType &&
              types.functions.at(lowered.destroy).result == Unit,
          "frame helper ownership signature differs");
  auto checked = core::verify(p, {true});
  bool heapOwned = false, callerDead = false;
  for (const auto &s : checked.get().captureStates)
    if (s.function == witness.function) {
      bool handle = false, original = false;
      for (const auto &r : s.resources)
        if (r.location.kind == storage::Location::Symbol) {
          handle |= r.location.id == lowered.heapStorage && r.memory == MemoryState::Own;
          original |= lowered.restoredSymbols.contains(r.location.id);
        }
      heapOwned |= handle;
      callerDead |= handle && !original;
    }
  require(heapOwned && callerDead, "heap handoff did not consume original bindings");

  unsigned rejected = 0;
  auto reject = [&](auto change) {
    auto out = make();
    change(out);
    try {
      (void)core::verify(std::move(out.program));
      throw std::runtime_error("invalid heap frame Core accepted");
    } catch (const Error &) {
      ++rejected;
    }
  };
  reject([&](auto &out) {
    auto &body = function(out.program, out.factory).body;
    auto again = body[0];
    again.result = out.program.values++;
    body.insert(body.begin() + 1, again);
  });
  reject([&](auto &out) {
    auto &body = function(out.program, out.destroy).body;
    body.insert(body.begin() + 2, body[1]);
  });
  reject([&](auto &out) {
    function(out.program, out.restore).body[0].intrinsicId = IntrinsicId::Box_new_generic;
  });
  reject([&](auto &out) {
    auto &body = function(out.program, out.factory).body;
    body.back().args = {body.front().result};
  });
  reject([&](auto &out) {
    auto &body = function(out.program, out.restore).body;
    body.back().args = {function(out.program, out.restore).parameters[0]};
  });
  reject([&](auto &out) {
    auto &instances = out.program.semantics.instances;
    for (auto it = instances.begin(); it != instances.end(); ++it)
      if (it->type == out.heapType) {
        instances.erase(it);
        return;
      }
    throw std::runtime_error("missing instance mutation");
  });
  reject([&](auto &out) { out.program.semantics.types[out.heapType].element = RawPtr; });
  reject(
      [&](auto &out) { out.program.semantics.functions[out.factory].kind = FunctionKind::Async; });
  reject([&](auto &out) {
    auto &body = function(out.program, witness.function).body;
    for (size_t n = 0; n < body.size(); ++n)
      if (body[n].kind == core::Inst::Call && body[n].function == out.restore) {
        auto again = body[n];
        again.result = out.program.values++;
        body.insert(body.begin() + n + 1, again);
        return;
      }
    throw std::runtime_error("missing restore mutation");
  });
  reject([&](auto &out) {
    auto &body = function(out.program, witness.function).body;
    for (size_t n = 0; n < body.size(); ++n)
      if (body[n].kind == core::Inst::Bind && body[n].slot == out.heapStorage) {
        body.insert(body.begin() + n + 1, body[n]);
        return;
      }
    throw std::runtime_error("missing heap binding mutation");
  });
  require(rejected == 10, "missing heap Core corruption coverage");

  for (uint32_t prefix = 0; prefix <= schema.slots.size(); ++prefix) {
    auto out = frames::lowerRoundTrip(source, witness, schema, {{prefix}, true});
    bool makeCall = false, restoreCall = false, destroyCall = false;
    for (const auto &i : function(out.program, witness.function).body)
      if (i.kind == core::Inst::Call) {
        makeCall |= i.function == out.factory;
        restoreCall |= i.function == out.restore;
        destroyCall |= i.function == out.destroy;
      }
    require(!restoreCall && makeCall == (prefix == schema.slots.size()) && destroyCall == makeCall,
            "partial frame escaped or cleanup resumed a consumed continuation");
    (void)core::verify(std::move(out.program));
  }

  auto permuted = schema;
  auto rename = [&](uint32_t id) { return static_cast<uint32_t>(schema.slots.size() - 1 - id); };
  std::reverse(permuted.slots.begin(), permuted.slots.end());
  for (auto &slot : permuted.slots)
    slot.id = rename(slot.id);
  for (auto &op : permuted.construct)
    if (op.slot != None)
      op.slot = rename(op.slot);
  for (auto &edge : permuted.cleanup)
    for (auto &op : edge.body)
      if (op.slot != None)
        op.slot = rename(op.slot);
  auto reordered = frames::lowerRoundTrip(source, witness, permuted, {{}, true});
  const auto &fields = reordered.program.semantics.types[reordered.frameType].fields;
  const auto &canonical = lowered.program.semantics.types[lowered.frameType].fields;
  require(fields.size() == canonical.size(), "schema permutation lost fields");
  for (size_t n = 0; n < fields.size(); ++n) {
    require(fields[n].name == "slot" + std::to_string(rename(static_cast<uint32_t>(n))) &&
                fields[n].type == canonical[n].type && fields[n].offset == canonical[n].offset,
            "permuting schema changed checked destruction order");
  }
  (void)core::verify(std::move(reordered.program));
  for (const char *name : {"frame_heap/valid_nested.isa", "frame_heap/valid_containers.isa",
                           "frame_heap/valid_copy.isa", "frame_heap/valid_regions.isa",
                           "frame_heap/valid_branches.isa"}) {
    auto fixture = load(name);
    const auto &s = marker(fixture.get());
    auto out = frames::roundTrip(fixture, s, frames::build(fixture, s).get(), {{}, true}, {true});
    (void)erase(out);
  }
  for (const char *name : {"frame_heap/blocked_protocol.isa", "frame_heap/blocked_borrow.isa",
                           "frame_heap/blocked_partial.isa", "frame_storage/valid_owned.isa"}) {
    auto fixture = load(name);
    const auto &s = marker(fixture.get());
    try {
      (void)frames::roundTrip(fixture, s, frames::build(fixture, s).get(), {{}, true});
      throw std::runtime_error("unsupported heap capture accepted");
    } catch (const Error &) {
    }
  }
  auto empty =
      core::verify(core::elaborate(resolve(
                       modules::compile(lex("@import \"std.box\"\nfn main(){print(7);}")).ast)),
                   {true});
  bool testedEmpty = false;
  for (const auto &s : empty.get().captureStates)
    if (s.function == empty.get().semantics.entryPoint.function &&
        s.side == core::StateSide::Before && s.cleanupValid && s.resources.empty()) {
      auto out = frames::lowerRoundTrip(empty, s, frames::build(empty, s).get(), {{0}, true});
      require(out.heapType == None && out.heapStorage == None && out.factory == None,
              "empty frame allocated heap storage");
      (void)core::verify(std::move(out.program));
      testedEmpty = true;
      break;
    }
  require(testedEmpty, "missing empty heap exit coverage");
  for (const auto &s : empty.get().captureStates)
    if (s.cleanupValid && s.resources.empty() && !s.terminal) {
      auto construction = frames::build(empty, s);
      (void)frames::buildLifecycle(construction, false);
      (void)frames::buildLifecycle(construction, true);
      auto emptyGraph = frames::verifyLifecycleGraph(construction, split.get());
      require(emptyGraph.construction().get().slots.empty() &&
                  emptyGraph.blockFacts()[2].outgoing == std::set{S::Destroyed},
              "empty schema bypassed lifecycle branch verification");
      break;
    }

  auto pure = core::verify(core::elaborate(resolve(modules::compile(lex(R"(
    @import "std.box"
    fn wrapper()->i32 { return middle(); }
    fn middle()->i32 { return leaf(); }
    fn leaf()->i32 { let value=7; marker(); return value; }
    fn marker() {}
    fn main()->i32 { return wrapper(); }
  )"))
                                                       .ast)),
                           {true});
  for (const auto &f : pure.get().semantics.functions)
    if (f.name == "leaf" || f.name == "middle" || f.name == "wrapper" || f.name == "main")
      require(!f.effects.allocation, "pure source fixture already allocates");
  const auto &point = marker(pure.get());
  auto effectful = frames::roundTrip(pure, point, frames::build(pure, point).get(), {{}, true});
  for (const auto &f : effectful.get().semantics.functions)
    if (f.name == "leaf" || f.name == "middle" || f.name == "wrapper" || f.name == "main")
      require(f.effects.allocation, "heap allocation effect lost across transformed calls");
}
