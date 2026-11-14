#include "isac/concurrency.hpp"
#include "isac/erasure.hpp"
#include "isac/modules.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

int main(int argc, char **argv) {
  using namespace isac;
  using namespace isac::concurrency;
  if (argc != 2)
    return 1;
  auto require = [](bool ok, const char *message) {
    if (!ok)
      throw std::runtime_error(message);
  };
  auto program = [&](const char *name) {
    std::ifstream input(std::filesystem::path(argv[1]) / (std::string(name) + ".isa"));
    std::string source((std::istreambuf_iterator<char>(input)), {});
    require(!source.empty(), "missing fixture");
    return core::elaborate(resolve(modules::compile(lex(source)).ast));
  };
  auto erased = [](const auto &verified) {
    std::ostringstream out;
    runtime::print(erase(verified), out);
    return out.str();
  };
  auto location = [](const auto &r) { return std::pair{r.location.kind, r.location.id}; };
  auto is = [](const auto &p, const auto &r, const char *name) {
    return r.location.kind == storage::Location::Symbol &&
           p.semantics.symbols.at(r.location.id).name == name;
  };
  auto owned = core::verify(program("valid_owned_states"), {true});
  auto plain = core::verify(program("valid_owned_states"));
  require(plain.get().captureStates.empty(), "default compilation retained capture snapshots");
  require(erased(owned) == erased(plain), "capture retention changed operational semantics");
  Analysis analysis(owned);
  bool partial = false, removed = false, deadButOwned = false, temporary = false, moved = false;
  for (const auto &state : owned.get().captureStates) {
    auto observations =
        analysis.states(state.function, state.point,
                        state.side == core::StateSide::Before ? Side::Before : Side::After);
    require(std::find(observations.begin(), observations.end(), &state) != observations.end(),
            "query lost state alternative");
    require(!state.cleanupValid || !state.blocker, "success and failure coexist");
    require(state.cleanupValid || state.cleanup.empty(),
            "failed replay exposed a partial cleanup schedule");
    require(!state.terminal || (!state.cleanupValid && state.blocker),
            "terminal state granted continuation");
    std::set<std::pair<storage::Location::Kind, uint32_t>> unique;
    for (const auto &drop : state.cleanup) {
      require(unique.insert(location(drop)).second, "cleanup destroys a resource twice");
      auto r = std::find_if(state.resources.begin(), state.resources.end(),
                            [&](const auto &r) { return r.location == drop.location; });
      require(r != state.resources.end() && r->type == drop.type &&
                  r->allocation == drop.allocation && r->absentFields == drop.absentFields &&
                  r->movedPayloads == drop.movedPayloads,
              "cleanup lost exact initialization/ownership masks");
      if (is(owned.get(), *r, "partial")) {
        for (auto field : r->absentFields)
          require(drop.fields.fields.at(field).skip, "uninitialized field would be dropped");
        partial |= r->absentFields.size() == 2;
        removed |= r->absentFields.contains(0) && !r->absentFields.contains(1);
        deadButOwned |= r->allocation && !r->executionLive;
      }
      temporary |= drop.location.kind == storage::Location::Value && drop.allocation;
    }
    bool transferred = false, old = false;
    for (const auto &r : state.resources) {
      transferred |= is(owned.get(), r, "transferred");
      old |= is(owned.get(), r, "removed");
      if (r.memory == MemoryState::Own && owned.get().semantics.linear(r.type))
        require(r.mode == core::CaptureMode::Move, "linear resource mistaken for Copy capture");
    }
    moved |= transferred && !old;
  }
  require(partial && removed && deadButOwned && temporary && moved,
          "missing owned capture/partial state coverage");

  auto corrupt = program("valid_owned_states");
  auto forged = owned.get().captureStates.front();
  forged.cleanupValid = true;
  forged.resources.clear();
  corrupt.captureStates = {forged};
  auto regenerated = core::verify(corrupt, {true});
  require(regenerated.get().captureStates.size() == owned.get().captureStates.size() &&
              !regenerated.get().captureStates.front().resources.empty(),
          "producer snapshot was trusted");
  require(core::verify(std::move(corrupt)).get().captureStates.empty(),
          "disabled retention kept forged witnesses");
  try {
    (void)Analysis(plain).states(owned.get().captureStates.front().function,
                                 owned.get().captureStates.front().point);
    throw std::runtime_error("absent observations treated as empty proof");
  } catch (const Error &e) {
    require(e.code == "E9001", "wrong missing witness diagnostic");
  }

  auto protocol = core::verify(program("valid_protocol_states"), {true});
  bool nonfinal = false, released = false, nested = false;
  for (const auto &state : protocol.get().captureStates) {
    if (state.blocker && state.blocker->code == DiagnosticCode::NonFinalState) {
      nonfinal = true;
      for (const auto &r : state.resources)
        if (is(protocol.get(), r, "outer")) {
          auto inner = r.fields.find(0);
          nested |= inner != r.fields.end() && !inner->second.fields.empty();
        }
    }
    for (const auto &r : state.resources)
      if (is(protocol.get(), r, "extracted") && !r.allocation)
        released |= state.cleanupValid && r.protocolState != None;
  }
  require(nonfinal && released && nested,
          "protocol/drop state reset to initial or lost recursively");

  auto payload = core::verify(program("valid_payload_states"), {true});
  bool payloadMoved = false;
  for (const auto &state : payload.get().captureStates)
    if (state.cleanupValid)
      for (const auto &drop : state.cleanup)
        payloadMoved |= !drop.movedPayloads.empty();
  require(payloadMoved, "destructured enum would drop an already transferred payload");

  auto stored = core::verify(program("valid_stored_reference_gate"), {true});
  bool sharedCarrier = false, uniqueCarrier = false;
  for (const auto &state : stored.get().captureStates)
    for (const auto &r : state.resources)
      if (!r.carriers.empty()) {
        require(!state.cleanupValid && state.blocker && state.cleanup.empty(),
                "stored reference bypassed frame storage contract");
        for (const auto &[path, carrier] : r.carriers) {
          (void)path;
          require(!carrier.origins.empty(), "capture lost recursive provenance");
          sharedCarrier |= carrier.capability == Access::Shared;
          uniqueCarrier |= carrier.capability == Access::Unique;
        }
      }
  require(sharedCarrier && uniqueCarrier, "mixed capabilities were flattened in capture state");

  auto spans = core::verify(program("valid_span_contracts"), {true});
  Analysis spanAnalysis(spans);
  bool local = false, parameter = false, shared = false, exclusive = false, backing = false;
  for (const auto &state : spans.get().captureStates)
    if (!state.terminal)
      for (const auto &r : state.resources)
        if (r.location.kind == storage::Location::Symbol &&
            spans.get().semantics.types.at(r.type).kind == hir::Type::Slice) {
          auto contract = spanAnalysis.span(state, r.location.id);
          require(!contract.regions.empty() && !state.cleanupValid && state.blocker,
                  "borrowed Span granted frame permission or lost region contract");
          local |= !contract.callerOwned && !contract.origins.empty() && !contract.loans.empty();
          parameter |= contract.callerOwned;
          shared |= contract.capability == Access::Shared;
          exclusive |= contract.capability == Access::Unique;
          for (const auto &[id, storage] : contract.storage) {
            (void)id;
            backing |= storage.backing && !storage.candidates.unknown &&
                       !storage.candidates.versions.empty();
          }
          auto copied = state;
          try {
            (void)spanAnalysis.span(copied, r.location.id);
            throw std::runtime_error("forged/foreign state accepted");
          } catch (const Error &e) {
            require(e.code == "E9001", "wrong forged Span diagnostic");
          }
        }
  require(local && parameter && shared && exclusive && backing,
          "Span contract coverage incomplete");

  auto entries = core::verify(program("valid_protocol_entries"), {true});
  std::map<std::pair<FunctionId, uint32_t>, std::set<StateId>> alternatives;
  for (const auto &state : entries.get().captureStates)
    if (state.side == core::StateSide::Before)
      alternatives[{state.function, state.point.block}].insert(state.entryState);
  require(std::any_of(alternatives.begin(), alternatives.end(),
                      [](const auto &x) { return x.second.size() == 2; }),
          "protocol entry alternatives collapsed into one state");
  auto loops = core::verify(program("valid_loop_edges"), {true});
  unsigned terminalDrops = 0;
  for (const auto &state : loops.get().captureStates)
    if (state.terminal)
      for (const auto &drop : state.completedCleanup)
        terminalDrops += drop.allocation;
  require(terminalDrops >= 3, "continue/break/return lost their actual cleanup effects");
}
