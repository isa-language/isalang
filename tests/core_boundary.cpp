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
      if (i.kind == core::Inst::BorrowShared && !i.payloadPath.empty()) {
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
  auto mutableAccessCore = [] {
    return core::elaborate(
        resolve(loadImports(parse(lex("@import \"std.box\" @import \"std.vec\" fn main(){"
                                      "let b=Box<i32>::new(42);let r=b.get_mut();r=7;print(r);"
                                      "let v=Vec<i32>::new();v.push(1);match v.get_mut(0){"
                                      "Some(x)=>{x=8;print(x);},None=>{}}"
                                      "match v.first_mut(){Some(x)=>{x=10;},None=>{}}"
                                      "match v.last_mut(){Some(x)=>{x=11;},None=>{}}"
                                      "let s=v.as_mut_slice();s[0]=9;print(s[0]);"
                                      "let a=v.as_slice();print(a[0]);}")))));
  };
  (void)core::verify(mutableAccessCore());
  for (int mode = 0; mode < 10; ++mode) {
    auto forged = mutableAccessCore();
    bool changed = false;
    if (mode < 7) {
      for (auto &f : forged.semantics.functions) {
        if (!containerMutableBorrowIntrinsic(f.intrinsicId))
          continue;
        if (mode == 0)
          f.resultAccess = Access::Shared;
        if (mode == 1)
          f.resultOrigin = None;
        if (mode == 2)
          f.parameterRegions.clear();
        if (mode == 3)
          f.parameterRegions[0].kind = BorrowKind::Shared;
        if (mode == 4)
          f.access = Access::Shared;
        if (mode == 5)
          f.consumesSelf = true;
        if (mode == 6)
          f.additionalResultOrigins.push_back(f.params[0]);
        changed = true;
      }
    } else if (mode == 7) {
      for (auto &t : forged.semantics.types)
        if (t.kind == hir::Type::Reference && t.referenceAccess == Access::Unique) {
          t.referenceAccess = Access::Shared;
          changed = true;
        }
    } else {
      for (auto &f : forged.functions)
        for (auto &i : f.body)
          if (i.kind == core::Inst::BorrowUnique) {
            if (mode == 8)
              i.kind = core::Inst::BorrowShared;
            else
              forged.semantics.symbols[i.offset].access = Access::Shared;
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
  auto sliceAccessCore = [] {
    return core::elaborate(resolve(
        loadImports(parse(lex("fn edit(s:&mut i32[]){match s.get_mut(0){Some(x)=>{x=1;},None=>{}}"
                              "match s.first_mut(){Some(x)=>{x=2;},None=>{}}"
                              "match s.last_mut(){Some(x)=>{x=3;},None=>{}}}"
                              "fn read(s:&i32[])->Option<&i32>{return s.get(0);}"
                              "fn main(){let a=[1,2];edit(&mut a);"
                              "match a.first(){Some(x)=>{print(x);},None=>{}}"
                              "match a.last(){Some(x)=>{print(x);},None=>{}}"
                              "match read(&a){Some(x)=>{print(x);},None=>{}}}")))));
  };
  (void)core::verify(sliceAccessCore());
  for (int mode = 0; mode < 11; ++mode) {
    auto forged = sliceAccessCore();
    bool changed = false;
    for (auto &f : forged.semantics.functions) {
      if (!elementSliceIntrinsic(f.intrinsicId))
        continue;
      if (mode == 0)
        f.resultOrigin = None;
      if (mode == 1)
        f.resultRegion = None;
      if (mode == 2)
        f.parameterRegions.clear();
      if (mode == 3)
        f.access = f.access == Access::Shared ? Access::Unique : Access::Shared;
      if (mode == 4)
        f.resultAccess = f.resultAccess == Access::Shared ? Access::Unique : Access::Shared;
      if (mode == 5)
        f.consumesSelf = true;
      if (mode == 6)
        f.resultBorrowedContainer = false;
      if (mode == 7)
        f.additionalResultOrigins.push_back(f.params[0]);
      if (mode == 8) {
        f.intrinsicId = IntrinsicId::Vec_get;
        f.intrinsic = "vec.get";
      }
      if (mode == 9) {
        if (f.params.size() != 2)
          continue;
        forged.semantics.symbols[f.params[1]].type = I32;
      }
      if (mode == 10)
        forged.semantics.symbols[f.params[0]].reference = false;
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
  auto exclusiveResultCore = [] {
    return core::elaborate(resolve(
        loadImports(parse(lex("fn first(s:&mut i32[])->Option<&mut i32>{return s.first_mut();}"
                              "fn main(){let a=[1,2];let s:&mut i32[]=&mut a;"
                              "let o=first(s);match o{Some(r)=>{r=42;},None=>{}}print(a[0]);}")))));
  };
  (void)core::verify(exclusiveResultCore());
  for (int mode = 0; mode < 9; ++mode) {
    auto forged = exclusiveResultCore();
    auto found = std::find_if(forged.semantics.functions.begin(), forged.semantics.functions.end(),
                              [](const auto &f) { return f.name == "first"; });
    if (found == forged.semantics.functions.end())
      return 1;
    auto &f = *found;
    if (mode == 0)
      f.resultOrigin = None;
    if (mode == 1)
      f.resultRegion = None;
    if (mode == 2)
      f.resultAccess = Access::Shared;
    if (mode == 3)
      f.additionalResultOrigins.push_back(f.params[0]);
    if (mode == 4) {
      forged.semantics.symbols[f.params[0]].access = Access::Shared;
      f.parameterRegions[0].kind = BorrowKind::Shared;
    }
    if (mode == 5)
      f.resultBorrowedContainer = false;
    if (mode == 6)
      f.external = true;
    if (mode == 7)
      f.foreignC = true;
    if (mode == 8)
      f.resultPayloadSource = hir::PayloadResultSource{f.params[0], {0, 0}};
    try {
      (void)core::verify(std::move(forged));
      return 1;
    } catch (const Error &e) {
      if (e.code != "E9001")
        return 1;
    }
  }
  auto exclusiveProjectionCore = [] {
    return core::elaborate(resolve(loadImports(
        parse(lex("struct S{data:i32[2],}fn first(s:&mut S)->Option<&mut i32>{"
                  "let view:&mut i32[]=&mut s.data;return view.first_mut();}fn main(){}")))));
  };
  (void)core::verify(exclusiveProjectionCore());
  auto prematureExclusiveEnd = exclusiveProjectionCore();
  bool insertedEnd = false;
  for (auto &f : prematureExclusiveEnd.functions) {
    if (prematureExclusiveEnd.semantics.functions[f.id].name != "first")
      continue;
    SymbolId parent = None;
    for (const auto &i : f.body)
      if (i.kind == core::Inst::BorrowUnique)
        parent = i.offset;
    if (parent == None)
      return 1;
    auto returned = std::find_if(f.body.begin(), f.body.end(),
                                 [](const auto &i) { return i.kind == core::Inst::Return; });
    core::Inst end{};
    end.kind = core::Inst::EndBorrow;
    end.loc = returned->loc;
    end.slot = parent;
    end.type = Unit;
    end.quantity = Quantity::Omega;
    f.body.insert(returned, end);
    insertedEnd = true;
  }
  if (!insertedEnd)
    return 1;
  try {
    (void)core::verify(std::move(prematureExclusiveEnd));
    return 1;
  } catch (const Error &e) {
    if (e.code != "E0312")
      return 1;
  }
  auto inspectionCore = [] {
    return core::elaborate(resolve(loadImports(parse(
        lex("@import \"std.vec\" fn main(){let v=Vec<i32>::new();v.push(1);"
            "let item=v.first_mut();print(item.is_some());print(item.is_none());item.drop();}")))));
  };
  (void)core::verify(inspectionCore());
  for (int mode = 0; mode < 4; ++mode) {
    auto forged = inspectionCore();
    bool changed = false;
    for (auto &f : forged.semantics.functions) {
      if (f.payloadInspectionParameters.empty())
        continue;
      if (mode == 0)
        f.payloadInspectionParameters.clear();
      else if (mode == 1)
        f.payloadInspectionParameters.push_back(f.payloadInspectionParameters.front());
      else if (mode == 2)
        f.payloadInspectionParameters.front() = None;
      else
        f.external = true;
      changed = true;
      break;
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
  auto forgedPayloadRead = inspectionCore();
  bool readInserted = false;
  for (auto &f : forgedPayloadRead.functions) {
    const auto &signature = forgedPayloadRead.semantics.functions[f.id];
    if (signature.payloadInspectionParameters.empty())
      continue;
    core::Inst read{};
    read.kind = core::Inst::Read;
    read.slot = signature.payloadInspectionParameters.front();
    read.type = RawPtr;
    read.offset = 1;
    read.result = forgedPayloadRead.values++;
    read.quantity = Quantity::Omega;
    f.body.insert(f.body.begin(), read);
    readInserted = true;
    break;
  }
  if (!readInserted)
    return 1;
  try {
    (void)core::verify(std::move(forgedPayloadRead));
    return 1;
  } catch (const Error &e) {
    if (e.code != "E9001" ||
        std::string(e.what()).find("opaque payload inspection") == std::string::npos)
      return 1;
  }

  auto forgedExtraction = inspectionCore();
  bool extractionInserted = false;
  for (auto &f : forgedExtraction.functions) {
    const auto &signature = forgedExtraction.semantics.functions[f.id];
    if (signature.payloadInspectionParameters.empty() ||
        signature.name.find("::is_some") == std::string::npos)
      continue;
    auto input = signature.payloadInspectionParameters.front();
    auto alias = static_cast<SymbolId>(forgedExtraction.semantics.symbols.size());
    auto symbol = forgedExtraction.semantics.symbols[input];
    symbol.id = alias;
    symbol.name = "$forged_payload";
    symbol.type = I32;
    symbol.access = Access::Unique;
    symbol.receiver = symbol.parameter = false;
    symbol.reference = true;
    symbol.region = 100000;
    forgedExtraction.semantics.symbols.push_back(std::move(symbol));
    core::Inst refine{};
    refine.kind = core::Inst::RefineVariant;
    refine.slot = input;
    refine.type = signature.owner;
    refine.offset = 0;
    refine.quantity = Quantity::One;
    core::Inst borrow{};
    borrow.kind = core::Inst::BorrowUnique;
    borrow.slot = input;
    borrow.offset = alias;
    borrow.type = I32;
    borrow.payloadPath = hir::payloadPosition(0, 0);
    borrow.quantity = Quantity::One;
    borrow.region = 100000;
    f.body.insert(f.body.begin(), borrow);
    f.body.insert(f.body.begin(), refine);
    extractionInserted = true;
    break;
  }
  if (!extractionInserted)
    return 1;
  try {
    (void)core::verify(std::move(forgedExtraction));
    return 1;
  } catch (const Error &e) {
    if (e.code != "E0312")
      return 1;
  }
  auto storedReborrowCore = [] {
    return core::elaborate(resolve(
        loadImports(parse(lex("@import \"std.vec\" fn main(){let v=Vec<i32>::new();v.push(1);"
                              "let parent=&mut v;let item=parent.get_mut(0);"
                              "match item{Some(value)=>{value=2;},None=>{}}v.push(3);}")))));
  };
  (void)core::verify(storedReborrowCore());
  auto prematureEnd = storedReborrowCore();
  bool inserted = false;
  for (auto &f : prematureEnd.functions) {
    SymbolId parent = None;
    TypeId parentType = Unit;
    for (size_t n = 0; n < f.body.size(); ++n) {
      const auto &i = f.body[n];
      if (i.kind == core::Inst::BorrowUnique) {
        parent = i.offset;
        parentType = i.type;
      }
      if (i.kind == core::Inst::Bind && parent != None &&
          prematureEnd.semantics.containsReferences(i.type)) {
        core::Inst end{};
        end.kind = core::Inst::EndBorrow;
        end.type = parentType;
        end.quantity = Quantity::One;
        end.slot = parent;
        end.loc = i.loc;
        end.unconditional = true;
        f.body.insert(f.body.begin() + n + 1, end);
        inserted = true;
        break;
      }
    }
    if (inserted)
      break;
  }
  if (!inserted)
    return 1;
  try {
    (void)core::verify(std::move(prematureEnd));
    return 1;
  } catch (const Error &e) {
    if (e.code != "E0312")
      return 1;
  }
  auto repeatedArrayCore = [] {
    return core::elaborate(resolve(parse(lex("fn main() { let bytes = [0u8; 8]; }"))));
  };
  (void)core::verify(repeatedArrayCore());
  for (int mode = 0; mode < 6; ++mode) {
    auto forged = repeatedArrayCore();
    bool changed = false;
    for (auto &i : forged.functions[0].body) {
      if (i.kind != core::Inst::ArrayRepeat)
        continue;
      auto &array = forged.semantics.types[i.type];
      if (mode == 0)
        array.count = 0;
      if (mode == 1)
        array.width = 7;
      if (mode == 2)
        array.element = I32;
      if (mode == 3)
        i.args.push_back(i.args[0]);
      if (mode == 4)
        i.quantity = Quantity::Omega;
      if (mode == 5)
        array.count = array.width = 65537;
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
  auto systemCore = [] {
    return core::elaborate(resolve(loadImports(
        parse(lex("@import \"std.fs\" fn main(){ let r=read_to_string(\"missing\"); }")))));
  };
  (void)core::verify(systemCore());
  for (int mode = 0; mode < 5; ++mode) {
    auto forged = systemCore();
    bool changed = false;
    for (auto &f : forged.semantics.functions) {
      if (f.intrinsicId != IntrinsicId::String_read_file)
        continue;
      if (mode == 0)
        f.access = Access::Shared;
      if (mode == 1)
        f.trusted = false;
      if (mode == 2)
        forged.semantics.symbols[f.params[0]].reference = true;
      if (mode == 3)
        forged.semantics.symbols[f.params[1]].type = I32;
      if (mode == 4)
        f.consumesSelf = true;
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
  auto sharedPayloadReceiverCore = [] {
    const std::string code =
        "enum View<T> { Some(T), None, } "
        "impl<T> View<T> { fn present(shared self) -> bool { "
        "match &self { Some(_) => { return true; }, None => { return false; } } } } "
        "fn main() { let x = 1; let view: View<&i32> = View::Some(&x); "
        "print(view.present()); }";
    return core::elaborate(resolve(parse(lex(code))));
  };
  (void)core::verify(sharedPayloadReceiverCore());
  for (int mode = 0; mode < 5; ++mode) {
    auto forged = sharedPayloadReceiverCore();
    bool changed = false;
    for (auto &f : forged.semantics.functions) {
      if (f.payloadParameterRegions.empty())
        continue;
      auto &contract = f.payloadParameterRegions[0];
      auto &receiver = forged.semantics.symbols[contract.parameter];
      if (mode == 0) {
        receiver.access = Access::Unique;
        f.access = Access::Unique;
      } else if (mode == 1) {
        receiver.access = Access::Own;
        f.access = Access::Own;
        f.consumesSelf = true;
      } else if (mode == 2)
        receiver.reference = true;
      else if (mode == 3)
        f.payloadParameterRegions.clear();
      else
        forged.semantics.symbols[contract.origin].access = Access::Unique;
      changed = true;
      break;
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
  const std::string aggregateSyntax =
      "struct Pair { left: i32, right: bool, } "
      "fn main() { let p = Pair { right: true, left: 3 }; "
      "let m: i32[2][3] = {{1,2,3},{4,5,6}}; let row = &m[1]; print(row[0]); }";
  auto aggregateCore = [&] { return core::elaborate(resolve(parse(lex(aggregateSyntax)))); };
  (void)core::verify(aggregateCore());
  for (unsigned mode = 0; mode != 6; ++mode) {
    auto forged = aggregateCore();
    bool changed = false;
    if (mode < 4) {
      for (auto &function : forged.functions)
        for (auto &inst : function.body)
          if (inst.kind == core::Inst::StructInit) {
            if (mode == 0)
              inst.fieldPath.pop_back();
            else if (mode == 1)
              inst.fieldPath[1] = inst.fieldPath[0];
            else if (mode == 2)
              inst.fieldPath[0] = None;
            else
              std::swap(inst.args[0], inst.args[1]);
            changed = true;
          }
    } else if (mode == 4) {
      for (auto &type : forged.semantics.types)
        if (type.kind == hir::Type::Array &&
            forged.semantics.types[type.element].kind == hir::Type::Array) {
          ++type.width;
          changed = true;
        }
    } else {
      for (auto &function : forged.functions)
        for (auto &inst : function.body)
          if (inst.kind == core::Inst::BorrowShared &&
              inst.placeProjection == core::Inst::PlaceProjection::Index) {
            inst.type = Bool;
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
  auto indexedReferenceCore = [] {
    return core::elaborate(resolve(parse(lex("fn main(){let a=[1,2];let parent=&mut a;"
                                             "let item=&mut parent[0];item=3;print(item);}"))));
  };
  (void)core::verify(indexedReferenceCore());

  for (unsigned mode = 0; mode != 3; ++mode) {
    auto forged = indexedReferenceCore();
    bool changed = false;
    for (auto &function : forged.functions)
      for (auto &inst : function.body) {
        if (inst.kind != core::Inst::BorrowUnique)
          continue;
        if (mode == 2 && inst.placeProjection == core::Inst::PlaceProjection::None) {
          inst.kind = core::Inst::BorrowShared;
          forged.semantics.symbols.at(inst.offset).access = Access::Shared;
          changed = true;
        }
        if (inst.placeProjection != core::Inst::PlaceProjection::Index)
          continue;
        if (mode == 0) {
          inst.type = Bool;
          changed = true;
        }
        if (mode == 1) {
          inst.kind = core::Inst::BorrowShared;
          forged.semantics.symbols.at(inst.offset).access = Access::Shared;
          changed = true;
        }
      }
    if (!changed)
      return 1;
    try {
      (void)core::verify(std::move(forged));
      return 1;
    } catch (const Error &) {
    }
  }
  auto directReborrowCore = [] {
    return core::elaborate(
        resolve(parse(lex("fn main(){let data=1;let parent=&mut data;let child=&mut *parent;"
                          "child=2;print(child);parent=3;}"))));
  };
  (void)core::verify(directReborrowCore());

  for (unsigned mode = 0; mode != 4; ++mode) {
    auto forged = directReborrowCore();
    SymbolId owner = None, parent = None, child = None;
    for (const auto &s : forged.semantics.symbols) {
      if (s.name == "data")
        owner = s.id;
      if (s.name == "parent")
        parent = s.id;
      if (s.name == "child")
        child = s.id;
    }
    bool changed = false;
    for (auto &f : forged.functions)
      for (auto &inst : f.body) {
        if (mode == 0 && inst.kind == core::Inst::BorrowUnique && inst.offset == parent) {
          inst.kind = core::Inst::BorrowShared;
          forged.semantics.symbols.at(parent).access = Access::Shared;
          changed = true;
        }
        if (mode == 1 && inst.kind == core::Inst::Write && inst.slot == child) {
          inst.slot = parent;
          changed = true;
        }
        if (inst.kind != core::Inst::BorrowUnique || inst.offset != child)
          continue;
        if (mode == 2) {
          inst.slot = owner;
          changed = true;
        }
        if (mode == 3) {
          inst.region = None;
          changed = true;
        }
      }
    if (!changed)
      return 1;
    try {
      (void)core::verify(std::move(forged));
      return 1;
    } catch (const Error &) {
    }
  }

  auto forgedEnds = directReborrowCore();
  for (auto &f : forgedEnds.functions)
    for (auto &inst : f.body)
      if (inst.kind == core::Inst::BorrowUnique)
        inst.inferredEnds.push_back(inst.slot);
  (void)core::verify(std::move(forgedEnds));
  auto storedSchemaCore = [] {
    return core::elaborate(
        resolve(parse(lex("struct Record{value:i32,} enum Holder<T>{Some(T),None,}"
                          "fn main(){let value=1;let wrapped=Holder::Some(&value);"
                          "match wrapped{Holder::Some(r)=>print(r),Holder::None=>{}}}"))));
  };
  (void)core::verify(storedSchemaCore());

  for (unsigned mode = 0; mode != 3; ++mode) {
    auto forged = storedSchemaCore();
    TypeId reference = None, holder = None, record = None;
    for (TypeId id = 0; id < forged.semantics.types.size(); ++id) {
      const auto &type = forged.semantics.types[id];
      if (type.kind == hir::Type::Reference)
        reference = id;
      if (type.kind == hir::Type::Enum && forged.semantics.containsReferences(id))
        holder = id;
      if (type.name == "Record")
        record = id;
    }
    if (reference == None || holder == None || record == None)
      return 1;
    if (mode == 0) {
      forged.semantics.types[record].fields[0].type = reference;
    } else if (mode == 1) {
      hir::Type nested{hir::Type::Enum, "ForgedNested"};
      nested.variants = {{0, "Nested", {holder}}};
      nested.width = 1 + forged.semantics.storageWidth(holder);
      forged.semantics.types.push_back(std::move(nested));
    } else {
      forged.semantics.types[reference].referenceAccess = Access::Own;
    }
    try {
      (void)core::verify(std::move(forged));
      return 1;
    } catch (const Error &error) {
      if (error.code != "E9001")
        return 1;
    }
  }
  std::cout << "Core gate rejects forged quantity, type and reference contracts\n";
}
