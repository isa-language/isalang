#include "isac/core.hpp"
#include "isac/erasure.hpp"
#include <algorithm>
#include <iostream>
#include <sstream>

int main() {
  using namespace isac;
  using namespace hir;
  const std::string source =
      "enum Choice<T>{Left(T),Right(T),Empty}"
      "struct Data{value:i32,}impl Data{pub fn field(shared self)->&i32{return &self.value;}}"
      "fn read(value:&i32)->i32{return value;}"
      "fn choose(a:&i32,b:&i32,right:bool)->Choice<&i32>{"
      "if right{return Choice::Right(b);}return Choice::Left(a);}"
      "fn inspect(value:Choice<&i32>){match value{"
      "Choice::Left(r)=>print(r),Choice::Right(r)=>print(r),Choice::Empty=>{}}}"
      "fn loop_refs(){let x=1;let i=0;while i<2{let v=Choice::Left(&x);match v{"
      "Choice::Left(r)=>{i=i+1;if i==1{continue;}print(r);if i==2{break;}},"
      "Choice::Right(r)=>print(r),Choice::Empty=>{}}}}"
      "fn test(flag:bool){let x=1;let r=&x;if flag{print(r);}"
      "let p=&mut x;p=2;let i=0;while i<2{let s=&x;print(s);i=i+1;}"
      "if flag{return;}print(x);}"
      "fn main(){let a=10;let b=20;let v=choose(&a,&b,true);inspect(v.move());test(false);"
      "let data=Data::new();let r=data.field();print(r);print(read(&a));loop_refs();}";
  auto make = [&] { return core::elaborate(resolve(parse(lex(source)))); };
  auto verified = core::verify(make());
  const auto &program = verified.get();
  size_t positions = 0, substitutions = 0, bounds = 0;
  bool differentPositions = false, multipleOrigins = false, ownerReceiver = false;
  for (const auto &graph : program.regionGraphs) {
    for (const auto &[parameter, arguments] : graph.contract.inputs) {
      std::set<RegionId> seen;
      for (const auto &[path, regions] : arguments) {
        if (regions.size() != 1 || !seen.insert(*regions.begin()).second)
          return 1;
        if (graph.regions.at(*regions.begin()).kind != regions::RegionKind::Parameter)
          return 2;
        ++positions;
      }
      differentPositions |= arguments.size() == 2 && seen.size() == 2;
    }
    for (const auto &call : graph.calls) {
      for (const auto &[formal, actual] : call.substitution)
        for (auto region : actual)
          if (!graph.regions.contains(region))
            return 3;
      for (const auto &[path, regions] : call.result)
        multipleOrigins |= regions.size() == 2;
      if (program.semantics.functions[call.target].name == "Data::field") {
        if (!call.result.contains({}) || call.result.at({}).empty())
          return 16;
        for (auto region : call.result.at({}))
          if (graph.regions.at(region).kind != regions::RegionKind::Storage ||
              !graph.ownerBounds.contains(region))
            return 17;
        ownerReceiver = true;
      }
      ++substitutions;
    }
    for (const auto &[region, owners] : graph.ownerBounds)
      for (auto owner : owners) {
        const auto &lower = graph.regions.at(region).points;
        const auto &upper = graph.storageBounds.at(owner).points;
        if (!std::includes(upper.begin(), upper.end(), lower.begin(), lower.end()))
          return 4;
        ++bounds;
      }
  }
  if (!positions || !substitutions || !bounds || !differentPositions || !multipleOrigins ||
      !ownerReceiver)
    return 5;

  auto invalid = program;
  bool forged = false;
  for (auto &graph : invalid.regionGraphs) {
    for (const auto &[region, owners] : graph.ownerBounds) {
      for (auto owner : owners) {
        const auto &upper = graph.storageBounds.at(owner).points;
        for (const auto &block : graph.blocks)
          if (block.reachable && !upper.contains(block.id)) {
            graph.regions.at(region).points.insert(block.id);
            auto before = upper;
            try {
              regions::validateBounds(invalid);
              return 6;
            } catch (const Error &error) {
              if (error.code != "E0310")
                return 7;
            }
            if (graph.storageBounds.at(owner).points != before)
              return 8;
            forged = true;
            break;
          }
        if (forged)
          break;
      }
      if (forged)
        break;
    }
    if (forged)
      break;
  }
  if (!forged)
    return 9;

  auto untrusted = make();
  untrusted.regionGraphs = invalid.regionGraphs;
  auto rebuilt = core::verify(std::move(untrusted));
  regions::validateBounds(rebuilt.get());
  auto badArguments = program;
  bool invalidPath = false;
  for (auto &graph : badArguments.regionGraphs)
    if (!graph.symbolRegions.empty()) {
      auto &[symbol, arguments] = *graph.symbolRegions.begin();
      arguments[{{ReferenceProjectionKind::Field, 999}}] = {graph.regions.begin()->first};
      try {
        regions::validateBounds(badArguments);
        return 18;
      } catch (const Error &error) {
        if (error.code != "E9001")
          return 19;
      }
      invalidPath = true;
      break;
    }
  if (!invalidPath)
    return 20;

  auto schemas = program.semantics;
  auto add = [&](Type type) {
    auto id = static_cast<TypeId>(schemas.types.size());
    schemas.types.push_back(std::move(type));
    return id;
  };
  Type reference{Type::Reference, "&i32"};
  reference.element = I32;
  auto ref = add(reference);
  Type pair{Type::Structure, "ViewSchema"};
  pair.fields = {{"a", ref, 99}, {"b", ref, 0}};
  auto view = add(pair);
  Type box{Type::Structure, "BoxSchema"};
  box.representation = Representation::GenericBox;
  box.element = view;
  auto boxed = add(box);
  Type vec{Type::Structure, "VecSchema"};
  vec.representation = Representation::GenericVec;
  vec.element = boxed;
  auto vector = add(vec);
  auto leaves = schemas.borrowedReferences(vector);
  if (leaves.size() != 2 || leaves[0].path.size() != 3)
    return 10;
  RegionArgs arguments{{leaves[0].path, {10}}, {leaves[1].path, {20}}};
  RegionSubstitution substitution{{10, {100, 200}}, {20, {300}}};
  auto actual = substituteRegions(arguments, substitution);
  if (actual.at(leaves[0].path) != std::set<RegionId>{100, 200} ||
      actual.at(leaves[1].path) != std::set<RegionId>{300})
    return 11;
  if (!substituteRegions(arguments, {{10, {}}, {20, {}}}).empty())
    return 21;
  auto moved = actual;
  if (moved != actual || schemas.containerElement(vector))
    return 12;
  try {
    (void)substituteRegions(arguments, {{10, {100}}});
    return 13;
  } catch (const Error &error) {
    if (error.code != "E9001")
      return 14;
  }

  std::ostringstream rich, erased;
  core::print(program, rich);
  print(erase(verified), erased);
  for (auto metadata : {"RegionArgs", "region substitution", "storage bound", "OwnerStorage"})
    if (rich.str().find(metadata) == std::string::npos ||
        erased.str().find(metadata) != std::string::npos)
      return 15;
  std::cout
      << "Positional RegionArgs, call substitution and immutable CFG storage bounds are consistent\n";
}
