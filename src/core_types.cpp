#include "isac/core.hpp"
#include <functional>
#include <map>

namespace isac::core {

void checkTypes(const Program &p) {
  const auto &entry = p.semantics.entryPoint;
  if (entry.function >= p.semantics.functions.size())
    fail({}, DiagnosticCode::InvalidCore, "missing semantic entrypoint");
  const auto &main = p.semantics.functions[entry.function];
  auto result = entry.signature == hir::MainSignatureKind::NoArgsUnit ? Unit : I32;
  if (!main.params.empty() || main.result != result || main.external)
    fail(main.loc, DiagnosticCode::InvalidCore, "entrypoint contract does not match its function");
  for (const auto &f : p.functions) {
    const auto &signature = p.semantics.functions.at(f.id);
    std::map<ValueId, TypeId> values;
    for (size_t n = 0; n < f.parameters.size(); ++n)
      values.emplace(f.parameters[n], p.semantics.symbols.at(signature.params.at(n)).type);
    std::function<void(const std::vector<Inst> &)> block;
    block = [&](const std::vector<Inst> &ops) {
      for (const auto &i : ops) {
        auto bad = [&]() {
          fail(i.loc, DiagnosticCode::InvalidCore, "invalid typed Core instruction");
        };
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
        auto q = i.kind == Inst::BorrowShared || i.kind == Inst::BorrowUnique ? Quantity::One
                 : i.kind == Inst::ProtocolTransition && i.type == Unit       ? Quantity::Zero
                 : p.semantics.linear(i.type)                                 ? Quantity::One
                                                                              : Quantity::Omega;
        if (i.quantity != q)
          bad();
        if (i.kind == Inst::Loop)
          block(i.condition);
        switch (i.kind) {
        case Inst::BorrowShared:
        case Inst::BorrowUnique: {
          if (i.offset >= p.semantics.symbols.size())
            bad();
          expect(p.semantics.symbols.at(i.offset).type, i.type);
          if (!i.projection)
            expect(slot(), i.type);
          else {
            const auto &t = p.semantics.types[slot()];
            if (t.kind != hir::Type::Enum)
              bad();
            bool found = false;
            for (const auto &variant : t.variants)
              for (size_t n = 0; n < variant.payload.size(); ++n)
                if (p.semantics.payloadOffset(slot(), variant.id, n) == i.projection &&
                    variant.payload[n] == i.type)
                  found = true;
            if (!found || p.semantics.types[i.type].protocol != None ||
                (i.kind == Inst::BorrowUnique && !p.semantics.heap(i.type)))
              bad();
          }
          break;
        }
        case Inst::RefineVariant:
          if (p.semantics.types[slot()].kind != hir::Type::Enum ||
              i.offset >= p.semantics.types[slot()].variants.size())
            bad();
          break;
        case Inst::StaticStr:
          expect(i.type, Str);
          if (!validUtf8(i.text))
            bad();
          break;
        case Inst::TextPrint:
          if (p.semantics.types[slot()].representation != hir::Representation::StringRecord)
            bad();
          break;
        case Inst::Constant:
          if (!scalar(i.type))
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
        case Inst::PayloadTake: {
          const auto &t = p.semantics.types[slot()];
          if (t.kind != hir::Type::Enum || !p.semantics.linear(i.type))
            bad();
          bool found = false;
          for (const auto &variant : t.variants)
            for (size_t n = 0; n < variant.payload.size(); ++n)
              if (p.semantics.payloadOffset(slot(), variant.id, n) == i.offset &&
                  variant.payload[n] == i.type)
                found = true;
          if (!found)
            bad();
          break;
        }
        case Inst::Read:
        case Inst::Write:
          if (i.offset >= p.semantics.types[slot()].width || (!scalar(i.type) && i.type != RawPtr))
            bad();
          if (i.kind == Inst::Write)
            expect(arg(0), i.type);
          break;
        case Inst::EnumConstruct: {
          auto &t = p.semantics.types[i.type];
          if (t.kind != hir::Type::Enum || i.offset >= t.variants.size())
            bad();
          auto &v = t.variants[i.offset];
          if (i.args.size() != v.payload.size())
            bad();
          for (size_t n = 0; n < i.args.size(); ++n)
            expect(arg(n), v.payload[n]);
          break;
        }
        case Inst::Create:
          if (p.semantics.types[i.type].kind != hir::Type::Structure)
            bad();
          break;
        case Inst::Cast: {
          auto from = arg(0);
          if (!(from == i.type || (from == Char && integer(i.type)) ||
                (numeric(from) && numeric(i.type) && !(floating(from) && integer(i.type)))))
            bad();
          break;
        }
        case Inst::Binary: {
          auto operand = arg(0);
          expect(arg(1), operand);
          if (i.text == "&&" || i.text == "||") {
            expect(operand, Bool);
            expect(i.type, Bool);
          } else if (i.text == "==" || i.text == "!=" || i.text == "<" || i.text == ">" ||
                     i.text == "<=" || i.text == ">=") {
            if (!scalar(operand))
              bad();
            expect(i.type, Bool);
          } else {
            if (!numeric(operand))
              bad();
            if ((i.text == "&" || i.text == "|" || i.text == "^" || i.text == "<<" ||
                 i.text == ">>") &&
                !integer(operand))
              bad();
            expect(i.type, operand);
          }
          break;
        }
        case Inst::Unary:
          expect(i.type, arg(0));
          if (i.text == "!")
            expect(i.type, Bool);
          else if (!numeric(i.type))
            bad();
          break;
        case Inst::Loop:
          if (!i.unconditional)
            expect(arg(0), Bool);
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
        case Inst::InputCall:
        case Inst::StringCall:
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
          if (i.intrinsicId != target.intrinsicId)
            bad();
          size_t skip = i.slot == None ? 0 : 1;
          if (i.args.size() + skip != target.params.size())
            bad();
          if (skip) {
            expect(slot(), p.semantics.symbols.at(target.params[0]).type);
            if (i.access != target.access)
              bad();
          }
          if (i.kind == Inst::InputCall &&
              (intrinsicInfo(target.intrinsicId).lowering != IntrinsicLowering::InputRecord ||
               i.text != target.intrinsic))
            bad();
          if (i.kind == Inst::StringCall &&
              (!target.intrinsic.starts_with("string.") || i.text != target.intrinsic))
            bad();
          for (size_t n = 0; n < i.args.size(); ++n)
            expect(arg(n), p.semantics.symbols.at(target.params[n + skip]).type);
          break;
        }
        case Inst::Print:
          if (!scalar(arg(0)) && arg(0) != Str)
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
        block(i.latch);
        values = before;
        block(i.otherwise);
        values = std::move(before);
      }
    };
    block(f.body);
  }
}
}
