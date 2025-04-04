#include "isac/core.hpp"
#include <functional>
#include <map>

namespace isac::core {

void checkTypes(const Program &p) {
  for (const auto &f : p.functions) {
    const auto &signature = p.semantics.functions.at(f.id);
    std::map<ValueId, TypeId> values;
    for (size_t n = 0; n < f.parameters.size(); ++n)
      values.emplace(f.parameters[n], p.semantics.symbols.at(signature.params.at(n)).type);
    std::function<void(const std::vector<Inst> &)> block;
    block = [&](const std::vector<Inst> &ops) {
      for (const auto &i : ops) {
        auto bad = [&]() { fail(i.loc, "E9001", "invalid typed Core instruction"); };
        auto expect = [&](TypeId got, TypeId wanted) {
          if (got != wanted)
            bad();
        };
        auto arg = [&](size_t n) {
          if (n >= i.args.size() || !values.contains(i.args[n]))
            bad();
          return values.at(i.args[n]);
        };
        auto slot = [&]() { return p.semantics.symbols.at(i.slot).type; };
        auto q = i.kind == Inst::ProtocolTransition && i.type == Unit ? Quantity::Zero
                 : p.semantics.linear(i.type)                         ? Quantity::One
                                                                      : Quantity::Omega;
        if (i.quantity != q)
          bad();
        switch (i.kind) {
        case Inst::Constant:
          if (i.type != I32 && i.type != Bool && i.type != USize)
            bad();
          break;
        case Inst::Copy:
          expect(slot(), i.type);
          if (p.semantics.linear(i.type))
            bad();
          break;
        case Inst::Move:
          expect(slot(), i.type);
          if (!p.semantics.linear(i.type))
            bad();
          break;
        case Inst::Bind:
          expect(slot(), i.type);
          expect(arg(0), i.type);
          break;
        case Inst::Array:
          if (p.semantics.types[i.type].kind != hir::Type::Array ||
              i.args.size() != p.semantics.types[i.type].count)
            bad();
          for (size_t n = 0; n < i.args.size(); ++n)
            expect(arg(n), p.semantics.types[i.type].element);
          break;
        case Inst::Read:
        case Inst::Write:
          if (i.offset >= p.semantics.types[slot()].width ||
              (i.type != I32 && i.type != Bool && i.type != USize && i.type != RawPtr))
            bad();
          if (i.kind == Inst::Write)
            expect(arg(0), i.type);
          break;
        case Inst::Create:
          if (p.semantics.types[i.type].kind != hir::Type::Structure)
            bad();
          break;
        case Inst::Binary: {
          auto operand = arg(0);
          expect(arg(1), operand);
          if (i.text == "&&" || i.text == "||") {
            expect(operand, Bool);
            expect(i.type, Bool);
          } else if (i.text == "+" || i.text == "-" || i.text == "*" || i.text == "/") {
            if (operand != I32 && operand != USize)
              bad();
            expect(i.type, operand);
          } else {
            expect(i.type, Bool);
            if (i.text != "==" && i.text != "!=") {
              if (operand != I32 && operand != USize)
                bad();
            } else if (operand != I32 && operand != Bool && operand != USize)
              bad();
          }
          break;
        }
        case Inst::Unary:
          expect(arg(0), i.text == "!" ? Bool : I32);
          expect(i.type, arg(0));
          break;
        case Inst::If:
          expect(arg(0), Bool);
          break;
        case Inst::Return:
          if (signature.result == Unit) {
            if (!i.args.empty())
              bad();
          } else
            expect(arg(0), signature.result);
          break;
        case Inst::HeapAlloc:
        case Inst::HeapFree:
        case Inst::HeapLoad:
        case Inst::HeapStore:
        case Inst::MemCopy:
        case Inst::MemMove:
        case Inst::Call:
        case Inst::ProtocolTransition: {
          const auto &target = p.semantics.functions.at(i.function);
          expect(i.type, target.result);
          size_t skip = i.slot == None ? 0 : 1;
          if (i.args.size() + skip != target.params.size())
            bad();
          if (skip)
            expect(slot(), p.semantics.symbols.at(target.params[0]).type);
          for (size_t n = 0; n < i.args.size(); ++n)
            expect(arg(n), p.semantics.symbols.at(target.params[n + skip]).type);
          break;
        }
        case Inst::Print:
          if (arg(0) != I32 && arg(0) != Bool && arg(0) != USize)
            bad();
          break;
        case Inst::Discard:
          expect(arg(0), i.type);
          break;
        default:
          break;
        }
        if (i.result != None && !values.emplace(i.result, i.type).second)
          bad();
        auto before = values;
        block(i.body);
        values = before;
        block(i.otherwise);
        values = std::move(before);
      }
    };
    block(f.body);
  }
}
}
