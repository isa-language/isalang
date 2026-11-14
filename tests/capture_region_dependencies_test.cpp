#include "isac/concurrency.hpp"
#include "isac/erasure.hpp"
#include "isac/frames.hpp"
#include "isac/modules.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <tuple>

int main(int argc, char **argv) {
  using namespace isac;
  using namespace isac::concurrency;
  using Location = storage::Location;
  if (argc != 2)
    return 1;
  auto require = [](bool condition, const char *message) {
    if (!condition)
      throw std::runtime_error(message);
  };
  auto program = [&](const char *name) {
    const auto path = std::filesystem::path(argv[1]) / (std::string(name) + ".isa");
    std::ifstream input(path);
    std::string source((std::istreambuf_iterator<char>(input)), {});
    require(!source.empty(), "missing regional dependency fixture");
    auto tokens = lex(source);
    for (auto &token : tokens)
      token.loc.file = path.string();
    return core::elaborate(resolve(modules::compile(tokens).ast));
  };
  auto erased = [](const auto &verified) {
    std::ostringstream out;
    runtime::print(erase(verified), out);
    return out.str();
  };
  auto reject = [&](auto action, const char *message) {
    try {
      action();
      throw std::runtime_error(message);
    } catch (const Error &error) {
      require(error.code == "E9001", "wrong regional witness diagnostic");
    }
  };
  bool shared = false, unique = false, parameter = false, unknownParent = false,
       unknownOrigins = false;
  bool missingParameterRegions = false;
  bool nested = false, storedParent = false, multipleOrigins = false, siblings = false;
  bool sparseCarriers = false, emptyCarriers = false, sparseStorage = false,
       observedStorage = false;
  bool observedSlots = false, backing = false, descriptor = false, referentSlot = false;
  bool ownerBound = false, projectionBound = false, internal = false, projectedInternal = false;
  bool emptyPayload = false, differingSides = false, terminal = false, gated = false;
  std::set<VariantId> choiceVariants;
  for (const char *fixture :
       {"direct", "recursive", "storage", "projections", "multiple_origins"}) {
    auto raw = program(fixture);
    auto verified = core::verify(raw, {true});
    auto plain = core::verify(std::move(raw));
    const auto originalErasure = erased(verified);
    require(originalErasure == erased(plain), "retention changed regional fixture semantics");
    Analysis analysis(verified);
    auto foreign = core::verify(program(fixture), {true});
    Analysis foreignAnalysis(foreign);
    std::map<std::tuple<FunctionId, uint32_t, StateId>, std::set<Location>> before;
    for (const auto &state : verified.get().captureStates) {
      auto copied = state;
      reject([&] { (void)analysis.captureRegions(copied); }, "copied regional witness accepted");
      reject([&] { (void)foreignAnalysis.captureRegions(state); },
             "foreign regional witness accepted");
      if (state.terminal) {
        reject([&] { (void)analysis.captureRegions(state); },
               "terminal edge supplied regional continuation");
        terminal = true;
        continue;
      }
      const auto &p = verified.get();
      const auto &graph =
          *std::find_if(p.regionGraphs.begin(), p.regionGraphs.end(),
                        [&](const auto &g) { return g.function == state.function; });
      const auto facts = analysis.captureRegions(state);
      require(facts.function == state.function && facts.point.block == state.point.block &&
                  facts.point.statementIndex == state.point.statementIndex &&
                  facts.side == state.side && facts.entryState == state.entryState &&
                  facts.proposedTransfers.empty(),
              "query changed boundary identity or merged protocol alternatives");
      const bool after = state.side == core::StateSide::After;
      const auto carrierObservation = graph.carriers.transitions.find(state.point.block);
      if (carrierObservation != graph.carriers.transitions.end()) {
        require(facts.carriers && *facts.carriers == (after ? carrierObservation->second.after
                                                            : carrierObservation->second.before),
                "query lost the original carrier observation or selected the wrong side");
      } else if (!graph.carriers.identities.empty()) {
        require(!facts.carriers, "sparse missing carrier observation became an empty proof");
        sparseCarriers = true;
      } else {
        require(facts.carriers && facts.carriers->empty(),
                "known empty carrier domain became unobserved");
        emptyCarriers = true;
      }
      const auto storageObservation = graph.storage.transitions.find(state.point.block);
      require(facts.storage.has_value() == (storageObservation != graph.storage.transitions.end()),
              "missing storage observation became an empty allocation state");
      if (facts.storage) {
        require(*facts.storage ==
                    (after ? storageObservation->second.after : storageObservation->second.before),
                "storage epochs selected the wrong side");
        observedStorage = true;
      } else
        sparseStorage = true;
      const auto slotObservation = graph.storage.slotTransitions.find(state.point.block);
      require(facts.slots.has_value() == (slotObservation != graph.storage.slotTransitions.end()),
              "missing slot observation became a content/presence proof");
      if (facts.slots) {
        require(*facts.slots ==
                    (after ? slotObservation->second.after : slotObservation->second.before),
                "slot generations selected the wrong side");
        observedSlots = true;
      }
      std::set<Location> positions;
      std::map<SymbolId, std::set<Location>> originPositions;
      for (const auto &dependency : facts.references) {
        require(positions.insert(dependency.position).second,
                "one semantic reference position was duplicated");
        const auto resource =
            std::find_if(state.resources.begin(), state.resources.end(), [&](const auto &r) {
              return r.location.kind == dependency.position.kind &&
                     r.location.id == dependency.position.id;
            });
        require(resource != state.resources.end(),
                "query invented a reference without an initialized capture resource");
        const auto &path = dependency.position.path;
        auto provenance = resource->carriers.find(path);
        if (provenance != resource->carriers.end()) {
          require(dependency.originsObserved && dependency.origins == provenance->second.origins &&
                      dependency.capability == provenance->second.capability && dependency.parent &&
                      *dependency.parent == provenance->second.parent,
                  "recursive reference position lost its capability, origins, or suspended parent");
          storedParent |= *dependency.parent != None;
          nested |= path.size() >= 3;
        } else {
          require(path.empty() && (resource->mode == core::CaptureMode::Shared ||
                                   resource->mode == core::CaptureMode::Unique),
                  "query inferred an initialized carrier from its type schema");
          if (!dependency.carrier) {
            require(!dependency.parent,
                    "direct alias invented an exact suspended-parent observation");
            unknownParent = true;
          }
          unknownOrigins |= !dependency.originsObserved;
        }
        for (const auto &[position, arguments] : dependency.regions) {
          require(position == path, "regions from sibling semantic positions were combined");
          for (auto region : arguments)
            require(graph.regions.contains(region), "undefined regional argument exposed");
        }
        require(dependency.regionsObserved || dependency.regions.empty(),
                "unobserved typed position acquired fabricated region arguments");
        shared |= dependency.capability == Access::Shared;
        unique |= dependency.capability == Access::Unique;
        multipleOrigins |= dependency.origins.size() > 1;
        for (auto origin : dependency.origins)
          originPositions[origin].insert(dependency.position);
        if (dependency.position.kind == Location::Symbol &&
            p.semantics.symbols.at(dependency.position.id).parameter && path.empty()) {
          require(dependency.callerOwned, "borrowed parameter lost its caller-owned observation");
          const auto id = dependency.position.id;
          const auto &signature = p.semantics.functions.at(state.function);
          const auto explicitRegion =
              std::find_if(signature.parameterRegions.begin(), signature.parameterRegions.end(),
                           [&](const auto &input) { return input.parameter == id; });
          const auto input = graph.contract.inputs.find(id);
          const bool hasRootContract =
              input != graph.contract.inputs.end() && input->second.contains(hir::ReferencePath{});
          if (hasRootContract) {
            require(dependency.regionsObserved && dependency.regions.contains(hir::ReferencePath{}),
                    "borrowed input position lost its retained region observation");
            for (auto region : input->second.at(hir::ReferencePath{}))
              require(dependency.regions.at(hir::ReferencePath{}).contains(region),
                      "borrowed input position lost one of its external bounds");
          }
          if (explicitRegion != signature.parameterRegions.end()) {
            require(
                hasRootContract && dependency.regionsObserved &&
                    dependency.regions.contains(hir::ReferencePath{}) &&
                    dependency.regions.at(hir::ReferencePath{}).contains(explicitRegion->region),
                "explicit borrowed parameter lost its external region contract");
            parameter = true;
          } else if (!hasRootContract) {

            require(!dependency.regionsObserved && dependency.regions.empty(),
                    "uncontracted parameter was given a fabricated external region contract");
            missingParameterRegions = true;
          }
        }
        if (p.semantics.types.at(resource->type).name.starts_with("Choice<"))
          for (const auto &part : path)
            if (part.kind == hir::ReferenceProjectionKind::VariantPayload)
              choiceVariants.insert(part.variant);
        for (const auto &owner : dependency.owners) {
          require(owner.relation == CaptureOwnerRelation::External &&
                      owner.proposedTransfers.empty(),
                  "an owner was classified as transferred without a proposal");
          require(owner.origin == dependency.origins.contains(owner.owner),
                  "origin and upper-bound dependencies were conflated");
          const auto bound = graph.storageBounds.find(owner.owner);
          require(owner.bound.has_value() == (bound != graph.storageBounds.end()),
                  "missing owner bound became a lifetime proof");
          if (owner.bound) {
            require(owner.bound->owner == bound->second.owner &&
                        owner.bound->points == bound->second.points,
                    "regional query grew an owner bound to cover its boundary");
            ownerBound = true;
          }
        }
        for (const auto &source : dependency.loans) {
          const auto loan = std::find_if(graph.loans.begin(), graph.loans.end(),
                                         [&](const auto &l) { return l.id == source.loan.id; });
          require(loan != graph.loans.end() && source.loan.region == loan->region &&
                      source.loan.place.root == loan->place.root &&
                      source.loan.points == loan->points &&
                      source.demanded == loan->points.contains(state.point.block),
                  "loan source became a dynamic presence proof");
          const auto observed = graph.storage.loans.find(loan->id);
          require(source.storage.has_value() == (observed != graph.storage.loans.end()),
                  "missing loan storage became known storage");
          if (source.storage) {
            require(*source.storage == observed->second,
                    "query changed backing epochs or referent slot generations");
            backing |= source.storage->backing;
            if (dependency.position.kind == Location::Symbol &&
                p.semantics.symbols.at(dependency.position.id).name == "descriptor")
              descriptor |= !source.storage->backing;
            referentSlot |= source.storage->slot.has_value();
          }
          for (const auto &projection : source.projections) {
            require(projection.loans.contains(loan->id) && !projection.fields.empty(),
                    "unrelated projection bound attached to loan");
            auto original = std::find_if(graph.projectionBounds.begin(),
                                         graph.projectionBounds.end(), [&](const auto &entry) {
                                           return entry.second.owner == projection.owner &&
                                                  entry.second.fields == projection.fields;
                                         });
            require(original != graph.projectionBounds.end() &&
                        projection.points == original->second.points,
                    "query widened a field presence bound");
            projectionBound = true;
          }
        }
      }
      for (const auto &[origin, references] : originPositions) {
        (void)origin;
        siblings |= references.size() >= 2;
      }
      for (const auto &resource : state.resources) {
        for (const auto &[path, provenance] : resource.carriers) {
          (void)provenance;
          require(positions.contains({resource.location.kind, resource.location.id, path}),
                  "nested field/enum carrier obligation disappeared from the query");
        }
        if (resource.location.kind == Location::Symbol &&
            p.semantics.symbols.at(resource.location.id).name == "empty") {
          require(resource.carriers.empty() &&
                      std::none_of(positions.begin(), positions.end(),
                                   [&](const auto &position) {
                                     return position.kind == resource.location.kind &&
                                            position.id == resource.location.id;
                                   }),
                  "reference-free variant acquired a fabricated carrier");
          emptyPayload = true;
        }
      }
      auto key = std::tuple{state.function, state.point.block, state.entryState};
      if (!after)
        before[key] = positions;
      else if (auto found = before.find(key); found != before.end())
        differingSides |= found->second != positions;
      if (!facts.references.empty()) {
        require(!state.cleanupValid && state.blocker && state.cleanup.empty(),
                "regional observations granted borrowed cancellation permission");
        try {
          (void)frames::build(verified, state);
          throw std::runtime_error("regional observations enabled borrowed frame storage");
        } catch (const Error &) {
          gated = true;
        }
        const auto &selected = facts.references.front();
        std::set<Location> transfers;
        for (const auto &owner : selected.owners)
          transfers.insert({Location::Symbol, owner.owner});
        for (const auto &dependency : facts.references)
          for (const auto &loan : dependency.loans)
            for (const auto &projection : loan.projections) {
              Location projected{Location::Symbol, projection.owner};
              for (auto field : projection.fields)
                projected.path.push_back({hir::ReferenceProjectionKind::Field, field});
              transfers.insert(std::move(projected));
            }
        const auto proposed = analysis.captureRegions(state, transfers);
        require(proposed.proposedTransfers == transfers &&
                    proposed.references.size() == facts.references.size() &&
                    proposed.carriers == facts.carriers && proposed.storage == facts.storage &&
                    proposed.slots == facts.slots,
                "a transfer proposal changed observed obligations or sparse storage facts");
        for (const auto &dependency : proposed.references)
          for (const auto &owner : dependency.owners) {
            std::set<Location> expected;
            for (const auto &transfer : transfers)
              if (transfer.kind == Location::Symbol && transfer.id == owner.owner)
                expected.insert(transfer);
            require(owner.proposedTransfers == expected &&
                        owner.relation == (expected.empty()
                                               ? CaptureOwnerRelation::External
                                               : CaptureOwnerRelation::PossibleInternal),
                    "possible self-reference was missed or an external origin was marked captured");
            internal |= !expected.empty();
            for (const auto &transfer : expected)
              projectedInternal |= !transfer.path.empty();
          }
      }
    }
    require(erased(verified) == originalErasure,
            "regional dependency query mutated verified Core or cleanup");
  }
  require(shared && unique && parameter && missingParameterRegions && unknownParent &&
              unknownOrigins,
          "direct-reference observation coverage incomplete");
  require(nested && storedParent && multipleOrigins && siblings && choiceVariants.size() == 2 &&
              emptyPayload,
          "recursive capability/origin/semantic-position coverage incomplete");
  require(sparseCarriers && emptyCarriers && sparseStorage && observedStorage && observedSlots,
          "sparse missing versus observed empty boundary coverage incomplete");
  require(backing && descriptor && referentSlot && ownerBound && projectionBound && internal &&
              projectedInternal,
          "owner, projected transfer, backing storage, or slot dependency coverage incomplete");
  require(differingSides && terminal && gated,
          "boundary identity or borrowed frame gates were not exercised");
}
