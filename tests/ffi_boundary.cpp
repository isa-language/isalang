#include "isac/core.hpp"
#include "isac/erasure.hpp"
#include <functional>
#include <iostream>
using namespace isac;
int main() {
  auto make = [] {
    return core::elaborate(
        resolve(parse(lex("extern \"C\" struct Data{tag:u8,value:i64,}"
                          "extern \"C\" fn pointer()->*mut Data;"
                          "fn main(){unsafe{let p=pointer();(*p).value=42;print((*p).value);}}"))));
  };
  auto verified = core::verify(make());
  if (erase(verified).cBridge.empty())
    return 1;
  for (int mode = 0; mode < 8; ++mode) {
    auto program = make();
    bool changed = false;
    std::function<void(std::vector<core::Inst> &)> visit = [&](auto &ops) {
      for (auto &op : ops) {
        if (mode == 0 && op.kind == core::Inst::UnsafeScope) {
          auto body = std::move(op.body);
          ops = std::move(body);
          changed = true;
          return;
        }
        if (mode == 1 && op.kind == core::Inst::ForeignCall) {
          op.kind = core::Inst::Call;
          changed = true;
        }
        if (mode == 2 && op.kind == core::Inst::RawStore) {
          op.fieldPath = {999};
          changed = true;
        }
        if (mode == 3 && op.kind == core::Inst::RawLoad) {
          op.type = I32;
          changed = true;
        }
        visit(op.body);
        visit(op.otherwise);
      }
    };
    if (mode <= 3)
      for (auto &f : program.functions)
        visit(f.body);
    if (mode == 4)
      for (auto &type : program.semantics.types)
        if (type.kind == hir::Type::RawPointer) {
          type.mutablePointer = false;
          changed = true;
        }
    if (mode == 5)
      for (auto &type : program.semantics.types)
        if (type.kind == hir::Type::ForeignRecord) {
          type.fields[1].offset = 1;
          changed = true;
        }
    if (mode == 6)
      for (auto &f : program.semantics.functions)
        if (f.foreignC) {
          f.foreignC = false;
          changed = true;
        }
    if (mode == 7)
      for (auto &f : program.semantics.functions)
        if (f.foreignC) {
          f.name = "isac_rt_alloc";
          changed = true;
        }
    if (!changed)
      return 1;
    try {
      (void)core::verify(std::move(program));
      return 1;
    } catch (const Error &e) {
      if (e.code != "E9001")
        return 1;
    }
  }

  for (int mode = 0; mode < 4; ++mode) {
    auto program = core::elaborate(
        resolve(parse(lex("extern \"C\" fn change(x:&mut i32[2]);"
                          "fn main(){let x:i32[2]=[1,2];unsafe{change(&mut x);}}"))));
    for (auto &f : program.semantics.functions)
      if (f.foreignC) {
        auto &param = program.semantics.symbols[f.params[0]];
        if (mode == 0)
          param.reference = false;
        if (mode == 1)
          param.access = Access::Shared;
        if (mode == 2)
          f.parameterRegions.clear();
        if (mode == 3)
          f.cLinkName = "change); injected(";
      }
    try {
      (void)core::verify(std::move(program));
      return 1;
    } catch (const Error &e) {
      if (e.code != "E9001")
        return 1;
    }
  }

  for (int mode = 0; mode < 12; ++mode) {
    auto program = core::elaborate(resolve(parse(
        lex("extern \"C\" union Number{integer:u64,decimal:f64,}"
            "extern \"C\" type Callback=extern \"C\" fn(i32)->i32;"
            "extern \"C\" fn callback(x:i32)->i32{return x+1;}"
            "extern \"C\" fn sum(n:i32,...)->i64;"
            "fn main(){let cb:Callback=&callback;unsafe{print(cb(1));print(sum(1,2u8));}}"))));
    bool changed = false;
    for (auto &type : program.semantics.types) {
      if (type.foreignUnion && mode == 0) {
        type.fields[1].offset = 4;
        changed = true;
      }
      if (type.foreignUnion && mode == 1) {
        type.width = 4;
        changed = true;
      }
      if (type.foreignUnion && mode == 2) {
        type.foreignUnion = false;
        changed = true;
      }
      if (type.kind == hir::Type::FunctionPointer && mode == 3) {
        type.parameters[0] = type.element = Bool;
        changed = true;
      }
      if (type.kind == hir::Type::FunctionPointer && mode == 4) {
        type.parameters[0] = &type - program.semantics.types.data();
        changed = true;
      }
    }
    for (auto &function : program.semantics.functions) {
      if (function.variadicDeclaration != None && mode == 5) {
        program.semantics.symbols[function.params.back()].type = U8;
        changed = true;
      }
      if (function.variadicDeclaration != None && mode == 6) {
        function.cLinkName = "other";
        changed = true;
      }
      if (function.variadicDeclaration != None && mode == 7) {
        function.variadicDeclaration = function.id;
        changed = true;
      }
    }
    std::function<void(std::vector<core::Inst> &)> visit = [&](auto &ops) {
      for (auto &op : ops) {
        if (op.kind == core::Inst::FunctionAddress && mode == 8) {
          op.type = USize;
          changed = true;
        }
        if (op.kind == core::Inst::IndirectForeignCall && mode == 9) {
          op.offset = Bool;
          changed = true;
        }
        if (op.kind == core::Inst::IndirectForeignCall && mode == 10) {
          op.args.pop_back();
          changed = true;
        }
        if (op.kind == core::Inst::UnsafeScope && mode == 11) {
          ops = std::move(op.body);
          changed = true;
          return;
        }
        visit(op.body);
        visit(op.otherwise);
      }
    };
    for (auto &function : program.functions)
      visit(function.body);
    if (!changed)
      return 1;
    try {
      (void)core::verify(std::move(program));
      return 1;
    } catch (const Error &e) {
      if (e.code != "E9001")
        return 1;
    }
  }
  std::cout << "C ABI, raw pointer and unsafe contracts checked before erasure\n";
}
