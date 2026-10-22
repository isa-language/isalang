#include "isac/core.hpp"
#include <iostream>

int main() {
  using namespace isac;
  using namespace hir;
  auto verified = core::verify(core::elaborate(resolve(parse(
      lex("fn identity(x:&i32)->&i32{return x;}"
          "fn main(){let data=1;let parent=&mut data;let reader=&*parent;print(reader);}")))));
  auto program = verified.get().semantics;
  auto add = [&](Type type) {
    auto id = static_cast<TypeId>(program.types.size());
    program.types.push_back(std::move(type));
    return id;
  };
  Type shared{Type::Reference, "&i32"};
  shared.element = I32;
  auto sharedId = add(shared);
  auto unique = shared;
  unique.name = "&mut i32";
  unique.referenceAccess = Access::Unique;
  auto uniqueId = add(unique);
  bool checkedShared = false, checkedUnique = false, checkedReturn = false;
  for (const auto &symbol : program.symbols) {
    auto reference = program.symbolReference(symbol.id);
    if (!symbol.reference) {
      if (reference)
        return 1;
      continue;
    }
    if (!reference || reference->region == None || reference->referent != I32)
      return 2;
    auto shape = reference->capability == Access::Shared ? sharedId : uniqueId;
    if (reference != program.referenceInfo(shape, symbol.region))
      return 3;
    checkedShared |= reference->capability == Access::Shared;
    checkedUnique |= reference->capability == Access::Unique;
  }
  for (const auto &function : program.functions)
    if (auto reference = program.functionReferenceResult(function.id)) {
      if (reference->region == None || reference->referent != I32 ||
          reference->capability != Access::Shared)
        return 4;
      checkedReturn = true;
    }
  if (!checkedShared || !checkedUnique || !checkedReturn)
    return 5;

  Type pair{Type::Structure, "PairView"};
  pair.fields = {{"left", sharedId, 900}, {"right", uniqueId, 17}};
  auto pairId = add(pair);
  auto refs = program.borrowedReferences(pairId);
  if (refs.size() != 2 ||
      refs[0].path != std::vector<ReferenceProjection>{{ReferenceProjectionKind::Field, 0}} ||
      refs[1].path != std::vector<ReferenceProjection>{{ReferenceProjectionKind::Field, 1}} ||
      !program.containsReferences(pairId) || program.referenceCapability(pairId, Access::Shared) ||
      program.referenceCapability(pairId, Access::Unique) ||
      program.supportsStoredReferenceReferent(pairId))
    return 6;
  Type variants{Type::Enum, "Nested"};
  variants.variants = {{41, "First", {pairId}}, {73, "Second", {sharedId, sharedId}}};
  auto enumId = add(variants);
  refs = program.borrowedReferences(enumId);
  if (refs.size() != 4 ||
      refs[0].path !=
          std::vector<ReferenceProjection>{{ReferenceProjectionKind::VariantPayload, 0, 41},
                                           {ReferenceProjectionKind::Field, 0}} ||
      refs[2].path !=
          std::vector<ReferenceProjection>{{ReferenceProjectionKind::VariantPayload, 0, 73}} ||
      refs[3].path !=
          std::vector<ReferenceProjection>{{ReferenceProjectionKind::VariantPayload, 1, 73}})
    return 7;
  Type box{Type::Structure, "BoxSchema"};
  box.representation = Representation::GenericBox;
  box.element = enumId;
  auto boxId = add(box);
  Type vector{Type::Structure, "VecSchema"};
  vector.representation = Representation::GenericVec;
  vector.element = boxId;
  auto vectorId = add(vector);
  refs = program.borrowedReferences(vectorId);
  if (refs.size() != 4 || refs[0].path.size() != 4 ||
      refs[0].path[0].kind != ReferenceProjectionKind::Element ||
      refs[0].path[1].kind != ReferenceProjectionKind::Element ||
      program.containerElement(vectorId) || program.supportsStoredReferenceReferent(vectorId))
    return 8;
  Type array{Type::Array, "ReferenceArraySchema"};
  array.element = sharedId;
  array.count = 50000;
  auto arrayId = add(array);
  refs = program.borrowedReferences(arrayId);
  if (refs.size() != 1 ||
      refs[0].path != std::vector<ReferenceProjection>{{ReferenceProjectionKind::Element}} ||
      !program.referenceCapability(arrayId, Access::Shared))
    return 9;
  Type slice{Type::Slice, "ReferenceSliceSchema"};
  slice.element = uniqueId;
  auto sliceId = add(slice);
  if (!program.containsReferences(sliceId) || !program.referenceCapability(sliceId, Access::Unique))
    return 10;
  Type raw{Type::RawPointer, "RawSchema"};
  raw.element = pairId;
  auto rawId = add(raw);
  Type callback{Type::FunctionPointer, "CallbackSchema"};
  callback.element = sharedId;
  callback.parameters = {pairId};
  auto callbackId = add(callback);
  if (program.containsReferences(rawId) || program.containsReferences(callbackId) ||
      program.referenceInfo(rawId))
    return 11;

  Type opaque = shared;
  opaque.element = pairId;
  auto opaqueId = add(opaque);
  if (program.borrowedReferences(opaqueId).size() != 1 ||
      !program.referenceCapability(opaqueId, Access::Shared))
    return 12;

  auto cycle = box;
  cycle.element = program.types.size();
  auto cycleId = add(cycle);
  if (program.containsReferences(cycleId))
    return 13;
  Type recordCycle{Type::Structure, "RecursiveSchema"};
  recordCycle.fields = {{"next", static_cast<TypeId>(program.types.size()), 0},
                        {"reference", sharedId, 1}};
  auto recordCycleId = add(recordCycle);
  if (program.borrowedReferences(recordCycleId).size() != 1)
    return 14;
  auto before = program.borrowedReferences(enumId);
  program.types.at(pairId).fields[0].offset = 0;
  program.types.at(pairId).fields[1].offset = 12345;
  if (program.borrowedReferences(enumId) != before)
    return 15;
  auto boxed = program.borrowedReferences(boxId);
  for (auto &reference : boxed)
    reference.path.erase(reference.path.begin());
  if (boxed != before)
    return 16;

  auto deep = I32;
  for (unsigned n = 0; n < 48; ++n) {
    Type repeated{Type::Structure, "SharedTypeDAG"};
    repeated.fields = {{"first", deep, 0}, {"second", deep, 1}};
    deep = add(repeated);
  }
  if (program.containsReferences(deep) || !program.referenceCapability(deep, Access::Shared))
    return 17;
  program.types[deep].fields.push_back({"borrow", uniqueId, 2});
  if (!program.containsReferences(deep) || program.referenceCapability(deep, Access::Shared))
    return 18;
  std::cout << "Reference shapes, regions and recursive semantic carrier paths are consistent\n";
}
