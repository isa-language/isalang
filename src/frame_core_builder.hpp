#pragma once
#include "isac/frames.hpp"

namespace isac::frames::detail {
using I = core::Inst;
struct CoreBuilder {
  core::Program &program;
  Loc origin;
  std::vector<I> body;
  I operation(I::Kind kind, TypeId type = Unit) const {
    I i{};
    i.kind = kind;
    i.type = type;
    i.loc = origin;
    i.quantity = program.semantics.linear(type) ? Quantity::One : Quantity::Omega;
    return i;
  }
  ValueId emit(I i, bool produces = false) {
    if (produces)
      i.result = program.values++;
    auto result = i.result;
    body.push_back(std::move(i));
    return result;
  }
  ValueId move(SymbolId symbol) {
    auto i = operation(I::Move, program.semantics.symbols.at(symbol).type);
    i.slot = symbol;
    return emit(i, true);
  }
  ValueId transfer(SymbolId symbol) {
    const auto type = program.semantics.symbols.at(symbol).type;
    auto i = operation(program.semantics.linear(type) ? I::Move : I::Copy, type);
    i.slot = symbol;
    return emit(i, true);
  }
  ValueId call(FunctionId target, std::vector<ValueId> args) {
    const auto &f = program.semantics.functions.at(target);
    auto i = operation(I::Call, f.result);
    i.function = target;
    i.intrinsicId = f.intrinsicId;
    i.args = std::move(args);
    return emit(i, f.result != Unit);
  }
  void bind(SymbolId symbol, ValueId value) {
    auto i = operation(I::Bind, program.semantics.symbols.at(symbol).type);
    i.slot = symbol;
    i.args = {value};
    emit(i);
  }
  void ret(TypeId type, ValueId value = None) {
    auto i = operation(I::Return, type);
    if (value != None)
      i.args = {value};
    emit(i);
  }
  ValueId construct(TypeId type, VariantId variant, std::vector<ValueId> args = {}) {
    auto i = operation(I::EnumConstruct, type);
    i.offset = variant;
    i.args = std::move(args);
    return emit(i, true);
  }
  ValueId test(SymbolId symbol, VariantId variant) {
    auto read = operation(I::Read, I64);
    read.slot = symbol;
    auto tag = emit(read, true);
    auto constant = operation(I::Constant, I64);
    constant.text = std::to_string(variant);
    auto expected = emit(constant, true);
    auto compare = operation(I::Binary, Bool);
    compare.text = "==";
    compare.operatorClass = hir::Expr::OperatorClass::Comparison;
    compare.args = {tag, expected};
    return emit(compare, true);
  }
  std::vector<ValueId> payloads(SymbolId symbol, VariantId variant,
                                const std::vector<TypeId> &types) {
    auto fact = operation(I::RefineVariant);
    fact.slot = symbol;
    fact.offset = variant;
    emit(fact);
    std::vector<ValueId> result;
    for (uint32_t n = 0; n < types.size(); ++n) {
      const auto type = types[n];
      auto take = operation(program.semantics.linear(type) ? I::PayloadTake : I::Read, type);
      take.slot = symbol;
      if (program.semantics.linear(type))
        take.payloadPath = hir::payloadPosition(variant, n);
      else
        take.offset =
            program.semantics.payloadOffset(program.semantics.symbols.at(symbol).type, variant, n);
      result.push_back(emit(take, true));
    }
    auto drop = operation(I::Drop, program.semantics.symbols.at(symbol).type);
    drop.slot = symbol;
    emit(drop);
    return result;
  }
  ValueId payload(SymbolId symbol, VariantId variant, TypeId type) {
    return payloads(symbol, variant, {type}).front();
  }
};
}
