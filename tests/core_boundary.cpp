#include "isac/core.hpp"
#include "isac/erasure.hpp"
#include <iostream>

int main() {
  using namespace isac;
  const std::string source = "fn main() -> i32 { let x = 1; return x; }";
  auto make = [&] { return core::elaborate(resolve(parse(lex(source)))); };
  auto valid = core::verify(make());
  if (erase(valid).functions.empty())
    return 1;
  auto bad = make();
  bad.functions[0].body[0].quantity = Quantity::One;
  try {
    auto ignored = core::verify(std::move(bad));
    (void)ignored;
    return 1;
  } catch (const Error &e) {
    if (e.code != "E9001")
      return 1;
  }
  bad = make();
  bad.functions[0].body[0].type = Bool;
  try {
    auto ignored = core::verify(std::move(bad));
    (void)ignored;
    return 1;
  } catch (const Error &e) {
    if (e.code != "E9001")
      return 1;
  }
  const std::string borrowed = "fn read(x: &i32) -> i32 { return x; } "
                               "fn main() -> i32 { let x = 1; return read(&x); }";
  auto referenceCore = [&] { return core::elaborate(resolve(parse(lex(borrowed)))); };
  (void)core::verify(referenceCore());
  auto missingContract = referenceCore();
  missingContract.semantics.functions[0].parameterRegions.clear();
  try {
    (void)core::verify(std::move(missingContract));
    return 1;
  } catch (const Error &e) {
    if (e.code != "E9001")
      return 1;
  }
  auto forgedReference = referenceCore();
  bool changed = false;
  for (auto &function : forgedReference.functions)
    for (auto &instruction : function.body)
      if (instruction.kind == core::Inst::ReferenceArgument) {
        instruction.access = Access::Unique;
        changed = true;
      }
  if (!changed)
    return 1;
  try {
    (void)core::verify(std::move(forgedReference));
    return 1;
  } catch (const Error &e) {
    if (e.code != "E9001")
      return 1;
  }
  auto forgedReceiver = make();
  forgedReceiver.semantics.functions[0].consumesSelf = true;
  try {
    (void)core::verify(std::move(forgedReceiver));
    return 1;
  } catch (const Error &e) {
    if (e.code != "E9001")
      return 1;
  }
  const std::string returning = "fn identity(x: &i32) -> &i32 { return x; } "
                                "fn main() { let x = 1; let r = identity(&x); print(r); }";
  auto resultCore = [&] { return core::elaborate(resolve(parse(lex(returning)))); };
  (void)core::verify(resultCore());
  auto missingResultContract = resultCore();
  missingResultContract.semantics.functions[0].resultOrigin = None;
  try {
    (void)core::verify(std::move(missingResultContract));
    return 1;
  } catch (const Error &e) {
    if (e.code != "E9001")
      return 1;
  }
  auto forgedResultFlavor = resultCore();
  forgedResultFlavor.semantics.functions[0].resultBorrowedContainer = true;
  try {
    (void)core::verify(std::move(forgedResultFlavor));
    return 1;
  } catch (const Error &e) {
    if (e.code != "E9001")
      return 1;
  }
  auto forgedResultOrigin = resultCore();
  forgedResultOrigin.semantics.functions[0].resultOrigin = UINT32_MAX - 1;
  try {
    (void)core::verify(std::move(forgedResultOrigin));
    return 1;
  } catch (const Error &e) {
    if (e.code != "E9001")
      return 1;
  }
  const std::string projecting =
      "struct Data { value: i32, } "
      "fn field(data: &Data) -> &i32 { return &data.value; } "
      "fn main() { let data = Data::new(); let r = field(&data); print(r); }";
  auto projectionCore = [&] { return core::elaborate(resolve(parse(lex(projecting)))); };
  (void)core::verify(projectionCore());
  auto forgedProjection = projectionCore();
  bool foundProjection = false;
  for (auto &function : forgedProjection.functions)
    for (auto &instruction : function.body)
      if (instruction.placeProjection == core::Inst::PlaceProjection::Field) {
        instruction.fieldIndex = UINT32_MAX;
        foundProjection = true;
      }
  if (!foundProjection)
    return 1;
  try {
    (void)core::verify(std::move(forgedProjection));
    return 1;
  } catch (const Error &e) {
    if (e.code != "E9001")
      return 1;
  }
  const std::string indexing =
      "fn at(values: &i32[], index: &usize) -> &i32 { return &values[index]; } "
      "fn main() {}";
  auto forgedIndex = core::elaborate(resolve(parse(lex(indexing))));
  auto &indexBody = forgedIndex.functions[0].body;
  bool foundIndex = false;
  for (size_t n = 0; n < indexBody.size(); ++n) {
    if (indexBody[n].placeProjection != core::Inst::PlaceProjection::Index)
      continue;
    core::Inst reference{};
    reference.kind = core::Inst::ReferenceArgument;
    reference.result = forgedIndex.values++;
    reference.type = USize;
    reference.slot = forgedIndex.semantics.functions[0].params[1];
    reference.access = Access::Shared;
    reference.quantity = Quantity::Omega;
    indexBody[n].args = {reference.result};
    indexBody.insert(indexBody.begin() + n, reference);
    foundIndex = true;
    break;
  }
  if (!foundIndex)
    return 1;
  try {
    (void)core::verify(std::move(forgedIndex));
    return 1;
  } catch (const Error &e) {
    if (e.code != "E9001")
      return 1;
  }
  const std::string methodReference =
      "struct Holder<T> { value: T, } "
      "impl<T> Holder<T> { fn value(shared self) -> &T { return &self.value; } } "
      "fn main() { let h = Holder<i32>::new(); let r = h.value(); print(r); }";
  auto methodCore = core::elaborate(resolve(parse(lex(methodReference))));
  (void)core::verify(methodCore);
  bool forgedMethodReceiver = false;
  for (const auto &signature : methodCore.semantics.functions)
    if (signature.returnsReference()) {
      methodCore.semantics.symbols[signature.resultOrigin].access = Access::Unique;
      forgedMethodReceiver = true;
    }
  if (!forgedMethodReceiver)
    return 1;
  try {
    (void)core::verify(std::move(methodCore));
    return 1;
  } catch (const Error &e) {
    if (e.code != "E9001")
      return 1;
  }
  auto payloadInputCore = [] {
    auto ast = parse(lex("struct Heap { ptr: RawPtr, } enum View<T> { Some(T), None, } "
                         "fn ignore(value: View<&Heap>) {} fn main() {}"));
    ast.structs[0].trusted = true;
    ast.structs[0].representation = "box_i32";
    return core::elaborate(resolve(ast));
  };
  (void)core::verify(payloadInputCore());
  for (int mode = 0; mode < 3; ++mode) {
    auto forged = payloadInputCore();
    auto &contracts = forged.semantics.functions[0].payloadParameterRegions;
    if (contracts.size() != 1)
      return 1;
    if (mode == 0)
      contracts.clear();
    else if (mode == 1)
      contracts[0].origin = contracts[0].parameter;
    else
      forged.semantics.symbols[contracts[0].origin].access = Access::Unique;
    try {
      (void)core::verify(std::move(forged));
      return 1;
    } catch (const Error &e) {
      if (e.code != "E9001")
        return 1;
    }
  }
  auto payloadResultCore = [] {
    auto ast = parse(lex("struct Heap { ptr: RawPtr, } enum View<T> { Some(T), None, } "
                         "fn forward(value: View<&Heap>) -> View<&Heap> { return value.move(); } "
                         "fn main() {}"));
    ast.structs[0].trusted = true;
    ast.structs[0].representation = "box_i32";
    return core::elaborate(resolve(ast));
  };
  (void)core::verify(payloadResultCore());
  for (int mode = 0; mode < 5; ++mode) {
    auto forged = payloadResultCore();
    auto &signature = forged.semantics.functions[0];
    if (!signature.resultPayloadSource)
      return 1;
    if (mode == 0)
      signature.resultPayloadSource.reset();
    else if (mode == 1)
      signature.resultPayloadSource->path.variant = UINT32_MAX;
    else if (mode == 2)
      signature.resultPayloadSource->path.field = UINT32_MAX;
    else if (mode == 3)
      signature.resultPayloadSource->parameter = UINT32_MAX;
    else
      signature.resultOrigin = signature.params[0];
    try {
      (void)core::verify(std::move(forged));
      return 1;
    } catch (const Error &e) {
      if (e.code != "E9001")
        return 1;
    }
  }
  auto multipleResultCore = [] {
    auto ast = parse(lex("struct Heap { ptr: RawPtr, } enum View<T> { Some(T), None, } "
                         "fn choose(a: View<&Heap>, b: View<&Heap>, flag: bool) -> View<&Heap> { "
                         "if flag { return a.move(); } return b.move(); } fn main() {}"));
    ast.structs[0].trusted = true;
    ast.structs[0].representation = "box_i32";
    return core::elaborate(resolve(ast));
  };
  (void)core::verify(multipleResultCore());
  for (int mode = 0; mode < 7; ++mode) {
    auto forged = multipleResultCore();
    auto &signature = forged.semantics.functions[0];
    if (signature.additionalResultOrigins.size() != 1 ||
        signature.additionalPayloadSources.size() != 1)
      return 1;
    if (mode == 0)
      signature.additionalResultOrigins[0] = UINT32_MAX;
    else if (mode == 1)
      signature.additionalResultOrigins[0] = signature.resultOrigin;
    else if (mode == 2)
      signature.additionalPayloadSources.clear();
    else if (mode == 3)
      signature.additionalPayloadSources[0].path.field = UINT32_MAX;
    else if (mode == 4)
      signature.additionalPayloadSources[0].parameter = UINT32_MAX;
    else if (mode == 5)
      forged.semantics.symbols[signature.additionalResultOrigins[0]].access = Access::Unique;
    else
      signature.additionalPayloadSources.push_back(signature.additionalPayloadSources[0]);
    try {
      (void)core::verify(std::move(forged));
      return 1;
    } catch (const Error &e) {
      if (e.code != "E9001")
        return 1;
    }
  }
  auto stackPayloadCore = [] {
    return core::elaborate(resolve(parse(
        lex("enum Choice<T> { View(T), Number(i32), } "
            "fn inspect(value: Choice<&i32>) { match &value { "
            "Choice::View(r) => print(r), Choice::Number(n) => print(n), } } fn main() {}"))));
  };
  auto stackProgram = stackPayloadCore();
  auto mark = [&](auto &&self, std::vector<core::Inst> &body) -> void {
    for (auto &i : body) {
      i.storedPayloadReference = true;
      self(self, i.body);
      self(self, i.otherwise);
      self(self, i.condition);
      self(self, i.latch);
    }
  };
  for (auto &f : stackProgram.functions)
    mark(mark, f.body);
  auto verifiedStack = core::verify(std::move(stackProgram));
  unsigned stored = 0, plain = 0;
  auto inspectProjection = [&](auto &&self, const std::vector<core::Inst> &body) -> void {
    for (const auto &i : body) {
      if (i.kind == core::Inst::BorrowShared && i.projection) {
        if (i.storedPayloadReference)
          ++stored;
        else
          ++plain;
      }
      self(self, i.body);
      self(self, i.otherwise);
      self(self, i.condition);
      self(self, i.latch);
    }
  };
  for (const auto &f : verifiedStack.get().functions)
    inspectProjection(inspectProjection, f.body);
  if (stored != 1 || plain != 1 || erase(verifiedStack).functions.empty())
    return 1;
  auto invalidStack = stackPayloadCore();
  for (auto &type : invalidStack.semantics.types)
    if (type.kind == hir::Type::Reference)
      type.element = RawPtr;
  try {
    (void)core::verify(std::move(invalidStack));
    return 1;
  } catch (const Error &e) {
    if (e.code != "E9001")
      return 1;
  }
  auto layoutCore = [&] {
    return core::elaborate(
        resolve(parse(lex("struct A { value: i32, } struct B { value: i32, } fn main() {}"))));
  };
  (void)core::verify(layoutCore());
  for (int corruption = 0; corruption < 3; ++corruption) {
    auto forged = layoutCore();
    TypeId a = None, b = None;
    for (TypeId id = 0; id < forged.semantics.types.size(); ++id) {
      if (forged.semantics.types[id].name == "A")
        a = id;
      if (forged.semantics.types[id].name == "B")
        b = id;
    }
    if (corruption == 0) {
      forged.semantics.types[a].fields[0].type = b;
      forged.semantics.types[b].fields[0].type = a;
    } else if (corruption == 1)
      forged.semantics.types[a].width = 9;
    else
      forged.semantics.types[a].fields[0].offset = 1;
    try {
      (void)core::verify(std::move(forged));
      return 1;
    } catch (const Error &e) {
      if (e.code != "E9001")
        return 1;
    }
  }
  auto fieldContractCore = [] {
    auto ast = parse(lex("struct Heap { ptr: RawPtr, } struct Holder { value: Heap, } "
                         "fn reset(h: &mut Holder) { let v=h.value.move(); h.value=v.move(); } "
                         "fn inspect(h: &Holder) { let r=&h.value; } fn main() {}"));
    ast.structs[0].trusted = true;
    ast.structs[0].representation = "box_i32";
    return core::elaborate(resolve(ast));
  };
  auto fieldContracts = fieldContractCore();
  for (auto &f : fieldContracts.functions)
    f.fieldPostStates.push_back({UINT32_MAX, {{UINT32_MAX, {}}}, {}});
  auto verifiedFields = core::verify(std::move(fieldContracts));
  unsigned summaries = 0;
  for (const auto &f : verifiedFields.get().functions)
    for (const auto &post : f.fieldPostStates) {
      ++summaries;
      if (post.parameter == UINT32_MAX || post.fields.size() != 1 || !post.fields.at(0).allocation)
        return 1;
    }
  if (summaries != 2)
    return 1;
  for (int mode = 0; mode < 3; ++mode) {
    auto forged = fieldContractCore();
    bool changed = false;
    for (auto &f : forged.functions)
      for (auto &i : f.body)
        if (i.kind == core::Inst::BorrowShared && !i.fieldPath.empty()) {
          if (mode == 0)
            i.fieldPath[0] = UINT32_MAX;
          if (mode == 1)
            i.fieldPath.clear();
          if (mode == 2)
            i.fieldIndex = UINT32_MAX;
          changed = true;
        }
    if (!changed)
      return 1;
    try {
      (void)core::verify(std::move(forged));
      return 1;
    } catch (const Error &e) {
      if (e.code != "E9001")
        return 1;
    }
  }
  auto storageCore = [] {
    return core::elaborate(
        resolve(loadImports(parse(lex("@import \"std.box\" @import \"std.vec\" fn main(){ "
                                      "let b=Box<i32>::new(42); print(b.into_inner()); "
                                      "let v=Vec<i32>::new(); v.push(7); let last=v.pop(); }")))));
  };
  (void)core::verify(storageCore());
  for (int mode = 0; mode < 7; ++mode) {
    auto forged = storageCore();
    bool changed = false;
    if (mode <= 1) {
      for (auto &t : forged.semantics.types)
        if (t.representation == hir::Representation::GenericVec) {
          if (mode == 0)
            t.element = RawPtr;
          else
            t.dropGlue = hir::DropGlue::FreeAllocation;
          changed = true;
        }
    } else if (mode == 2) {
      for (auto &f : forged.functions)
        for (auto &i : f.body)
          if (i.kind == core::Inst::ContainerCall) {
            i.kind = core::Inst::HeapAlloc;
            changed = true;
          }
    } else if (mode == 3 || mode == 4) {
      for (auto &f : forged.semantics.functions)
        if (f.intrinsicId == IntrinsicId::Box_into_inner) {
          if (mode == 3)
            f.consumesSelf = false;
          else
            f.access = Access::Shared;
          changed = true;
        }
    } else if (mode == 5) {
      for (auto &instance : forged.semantics.instances)
        if (forged.semantics.dynamicContainer(instance.type)) {
          instance.substitution.types[0].second = I64;
          changed = true;
        }
    }
    if (mode == 6) {
      for (auto &f : forged.functions)
        for (auto &i : f.body)
          if (i.kind == core::Inst::ContainerCall && i.intrinsicId == IntrinsicId::Vec_new) {
            i.kind = core::Inst::Create;
            changed = true;
          }
    }
    if (!changed)
      return 1;
    try {
      (void)core::verify(std::move(forged));
      return 1;
    } catch (const Error &e) {
      if (e.code != "E9001")
        return 1;
    }
  }
  auto accessCore = [] {
    return core::elaborate(
        resolve(loadImports(parse(lex("@import \"std.box\" @import \"std.vec\" fn main(){"
                                      "let b=Box<i32>::new(42); let r=b.get(); print(r);"
                                      "let v=Vec<i32>::new(); v.push(7); let o=v.get(0);"
                                      "match o {Some(x)=>{print(x);},None=>{}} }")))));
  };
  (void)core::verify(accessCore());
  for (int mode = 0; mode < 8; ++mode) {
    auto forged = accessCore();
    bool changed = false;
    for (auto &f : forged.semantics.functions) {
      if (f.intrinsicId != IntrinsicId::Box_get && f.intrinsicId != IntrinsicId::Vec_get)
        continue;
      if (mode == 0)
        f.resultOrigin = None;
      if (mode == 1)
        f.resultRegion = None;
      if (mode == 2)
        f.parameterRegions.clear();
      if (mode == 3)
        f.parameterRegions[0].kind = BorrowKind::Unique;
      if (mode == 4)
        f.access = Access::Unique;
      if (mode == 5)
        f.consumesSelf = true;
      if (mode == 6)
        f.additionalResultOrigins.push_back(f.params[0]);
      if (mode == 7 && f.intrinsicId == IntrinsicId::Vec_get)
        f.resultBorrowedContainer = false;
      changed = true;
    }
    if (!changed)
      return 1;
    try {
      (void)core::verify(std::move(forged));
      return 1;
    } catch (const Error &e) {
      if (e.code != "E9001")
        return 1;
    }
  }
  std::cout << "Core gate rejects forged quantity, type and reference contracts\n";
}
