#include "isac/core.hpp"
#include "isac/erasure.hpp"
#include "isac/modules.hpp"
#include <algorithm>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>

int main(int argc, char **argv) {
  using namespace isac;
  if (argc != 2)
    return 1;
  auto require = [](bool ok, const char *why) {
    if (!ok)
      throw std::runtime_error(why);
  };
  std::ifstream input(argv[1]);
  std::string source((std::istreambuf_iterator<char>(input)), {});
  auto make = [&] {
    auto tokens = lex(source);
    for (auto &token : tokens)
      token.loc.file = argv[1];
    return core::elaborate(resolve(modules::compile(tokens).ast));
  };
  auto verified = core::verify(make());
  const auto &p = verified.get();
  auto functionId = [&](const std::string &name) {
    auto it = std::find_if(p.semantics.functions.begin(), p.semantics.functions.end(),
                           [&](const auto &function) { return function.name == name; });
    require(it != p.semantics.functions.end(), "missing fixture function");
    return it->id;
  };
  auto first = functionId("get_number"), choose = functionId("choose"),
       equal = functionId("first_equal");
  auto graphFor = [&](FunctionId id) -> const regions::FunctionGraph & {
    auto it = std::find_if(p.regionGraphs.begin(), p.regionGraphs.end(),
                           [&](const auto &graph) { return graph.function == id; });
    require(it != p.regionGraphs.end(), "missing fixture graph");
    return *it;
  };
  const auto &f = p.semantics.functions[first];
  require(f.payloadParameterRegions.size() == 2, "input leaves were grouped into one origin");
  const auto &a = f.payloadParameterRegions[0], &b = f.payloadParameterRegions[1];
  require(a.parameter == b.parameter && a.origin != b.origin && a.region != b.region &&
              a.path != b.path,
          "semantic input positions are not independent");
  auto out = hir::payloadPosition(0, 0);
  auto transfers = p.semantics.referenceTransfers(f);
  require(transfers.size() == 1 &&
              transfers.at(out) == std::set<hir::ReferenceSource>{{a.parameter, a.path, a.origin}},
          "number result was bounded by unrelated text position");
  const auto &g = graphFor(first);
  require(g.contract.inputs.at(a.parameter).at(a.path) == std::set<RegionId>{a.region} &&
              g.contract.inputs.at(b.parameter).at(b.path) == std::set<RegionId>{b.region},
          "region input contract differs from provenance input contract");
  auto resultRegion = *g.contract.result.at(out).begin();
  require(g.contract.resultSources.at(resultRegion) == std::set<RegionId>{a.region},
          "region result source lost its semantic position");
  const auto &c = graphFor(choose);
  auto number = *c.contract.result.at(out).begin();
  auto text = *c.contract.result.at(hir::payloadPosition(1, 0)).begin();
  require(number != text &&
              c.contract.resultSources.at(number) != c.contract.resultSources.at(text),
          "different result positions share a flattened source set");
  require(p.semantics.referenceTransfers(p.semantics.functions[equal]).at(out).size() == 2,
          "signature lost conservative same-type origins");

  bool call = false, sharedCandidates = false, recursive = false, generic = false, pruned = false;
  for (const auto &graph : p.regionGraphs) {
    for (const auto &[value, definition] : graph.valueDefinitions)
      pruned |= graph.valueRegions.contains(value) &&
                !graph.resourcePoints.contains(definition.point.block);
    for (const auto &instantiation : graph.calls) {
      if (instantiation.target == first) {
        const auto &actual = instantiation.substitution.at(a.region);
        const auto &other = instantiation.substitution.at(b.region);
        require(actual.size() == 1 && other.size() == 1 && actual != other &&
                    instantiation.result.at(out) == actual,
                "call substituted unrelated reference positions");
        call = true;
      }
      recursive |= instantiation.target == graph.function &&
                   p.semantics.functions[graph.function].name == "relay";
      generic |=
          p.semantics.functions[instantiation.target].name.find("Transfer<") != std::string::npos;
    }
  }
  std::function<void(const std::vector<core::Inst> &)> inspect = [&](const auto &body) {
    for (const auto &i : body) {
      if (i.kind == core::Inst::Call && i.function == first) {
        require(i.carriers.at(out).origins.size() == 1, "resource result conflated input origins");
        auto owner = *i.carriers.at(out).origins.begin();
        require(p.semantics.symbols.at(owner).type == I32,
                "resource result retained unrelated String");
      }
      if (i.kind == core::Inst::Call && i.function == equal)
        sharedCandidates |= i.carriers.at(out).origins.size() == 2;
      inspect(i.body);
      inspect(i.otherwise);
      inspect(i.condition);
      inspect(i.latch);
    }
  };
  for (const auto &function : p.functions)
    inspect(function.body);
  require(call && sharedCandidates && recursive && generic && pruned,
          "missing call/composition/pruned-CFG coverage");

  auto schemas = p.semantics;
  auto add = [&](hir::Type type) {
    auto id = static_cast<TypeId>(schemas.types.size());
    schemas.types.push_back(std::move(type));
    return id;
  };
  hir::Type view{hir::Type::Structure, "ViewSchema"};
  view.fields = {{"number", *schemas.payloadType(schemas.symbols[a.parameter].type, a.path), 500},
                 {"text", *schemas.payloadType(schemas.symbols[b.parameter].type, b.path), 17}};
  auto viewId = add(view);
  hir::Type box{hir::Type::Structure, "BoxSchema"};
  box.representation = hir::Representation::GenericBox;
  box.element = viewId;
  auto boxed = add(box);
  hir::Type vec{hir::Type::Structure, "VecSchema"};
  vec.representation = hir::Representation::GenericVec;
  vec.element = boxed;
  auto vector = add(vec);
  auto recursiveSignature = f;
  recursiveSignature.result = vector;
  auto recursiveTransfers = schemas.referenceTransfers(recursiveSignature);
  auto leaves = schemas.borrowedReferences(vector);
  require(leaves.size() == 2 && leaves[0].path.size() == 3 &&
              recursiveTransfers.at(leaves[0].path) == transfers.at(out) &&
              recursiveTransfers.at(leaves[1].path) ==
                  std::set<hir::ReferenceSource>{{b.parameter, b.path, b.origin}} &&
              !schemas.borrowedInputSupported(vector) && !schemas.containerElement(vector),
          "recursive contract schema confused positions or authorized a layout");
  schemas.types[viewId].fields[0].offset = 17;
  require(schemas.referenceTransfers(recursiveSignature) == recursiveTransfers,
          "contract depends on physical offsets");
  auto parameter = schemas.symbols.at(a.parameter);
  parameter.id = schemas.symbols.size();
  parameter.type = vector;
  schemas.symbols.push_back(parameter);
  recursiveSignature.params = {parameter.id};
  recursiveSignature.payloadParameterRegions = {{parameter.id, a.origin, a.region, leaves[0].path},
                                                {parameter.id, b.origin, b.region, leaves[1].path}};
  auto recursiveInputs = schemas.referenceTransfers(recursiveSignature);
  require(recursiveInputs.at(leaves[0].path) ==
                  std::set<hir::ReferenceSource>{{parameter.id, leaves[0].path, a.origin}} &&
              recursiveInputs.at(leaves[1].path) ==
                  std::set<hir::ReferenceSource>{{parameter.id, leaves[1].path, b.origin}},
          "recursive input paths were flattened by the transfer contract");

  for (int corruption = 0; corruption != 5; ++corruption) {
    auto forged = make();
    auto &target = forged.semantics.functions[first];
    if (corruption == 0)
      target.payloadParameterRegions.pop_back();
    if (corruption == 1)
      target.payloadParameterRegions[1].path = target.payloadParameterRegions[0].path;
    if (corruption == 2) {
      auto &second = target.payloadParameterRegions[1];
      second.region = target.payloadParameterRegions[0].region;
      forged.semantics.symbols[second.origin].region = second.region;
    }
    if (corruption == 3)
      target.payloadParameterRegions[1].origin = target.payloadParameterRegions[0].origin;
    if (corruption == 4) {
      auto string = p.semantics.symbols.at(b.origin).type;
      auto &result = forged.semantics.types[target.result];
      result.variants[0].payload.push_back(string);
      ++result.width;
    }
    try {
      core::verify(std::move(forged));
      throw std::runtime_error("forged positional signature accepted");
    } catch (const Error &error) {
      require(error.code == "E9001", "wrong signature forgery diagnostic");
    }
  }
  for (int corruption = 0; corruption != 5; ++corruption) {
    auto forged = p;
    auto &graph = *std::find_if(forged.regionGraphs.begin(), forged.regionGraphs.end(),
                                [&](const auto &graph) { return graph.function == first; });
    if (corruption == 0)
      graph.contract.resultSources.at(resultRegion) = {b.region};
    else if (corruption == 1) {
      bool changed = false;
      for (auto &caller : forged.regionGraphs)
        for (auto &instantiation : caller.calls)
          if (instantiation.target == first) {
            instantiation.substitution[a.region] = instantiation.substitution.at(b.region);
            changed = true;
          }
      require(changed, "missing substitution to forge");
    } else if (corruption == 2) {
      for (auto &caller : forged.regionGraphs)
        std::erase_if(caller.calls, [&](const auto &call) { return call.target == first; });
    } else if (corruption == 3) {
      bool changed = false;
      for (auto &caller : forged.regionGraphs)
        for (auto &[value, definition] : caller.valueDefinitions)
          if (caller.valueRegions.contains(value) &&
              caller.resourcePoints.contains(definition.point.block)) {
            definition.type = Unit;
            changed = true;
            break;
          }
      require(changed, "missing typed definition to forge");
    } else
      graph.regions.at(a.region).kind = regions::RegionKind::Static;
    try {
      regions::validateBounds(forged);
      throw std::runtime_error("forged positional graph accepted");
    } catch (const Error &error) {
      require(error.code == "E9001", "wrong graph forgery diagnostic");
    }
    auto untrusted = make();
    untrusted.regionGraphs = forged.regionGraphs;
    regions::validateBounds(core::verify(std::move(untrusted)).get());
  }
  std::ostringstream erased;
  runtime::print(erase(verified), erased);
  for (auto marker : {"reference.transfer", "reference.input", "RegionArgs", "carrier.path"})
    require(erased.str().find(marker) == std::string::npos, "reference proof survived erasure");
}
