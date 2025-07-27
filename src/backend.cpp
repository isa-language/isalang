#include "isac/backend.hpp"
#include "isac/target.hpp"
#include "mlir/Conversion/ArithToLLVM/ArithToLLVM.h"
#include "mlir/Conversion/ControlFlowToLLVM/ControlFlowToLLVM.h"
#include "mlir/Conversion/FuncToLLVM/ConvertFuncToLLVMPass.h"
#include "mlir/Conversion/MemRefToLLVM/MemRefToLLVM.h"
#include "mlir/Conversion/ReconcileUnrealizedCasts/ReconcileUnrealizedCasts.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Target/LLVMIR/Dialect/Builtin/BuiltinToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Export.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include <stdexcept>
#include <unordered_map>
namespace isac {
namespace dialect {
using namespace mlir;
static bool buffer(Type t) {
  auto m = dyn_cast<MemRefType>(t);
  return m && m.getRank() == 1 && m.hasStaticShape() &&
         (m.getElementType().isInteger(32) || m.getElementType().isInteger(64));
}
#define ISAC_OP(CLASS, NAME, ...)                                                                  \
  class CLASS : public Op<CLASS, __VA_ARGS__> {                                                    \
  public:                                                                                          \
    using Op::Op;                                                                                  \
    static StringRef getOperationName() {                                                          \
      return NAME;                                                                                 \
    }                                                                                              \
    static ArrayRef<StringRef> getAttributeNames() {                                               \
      return {};                                                                                   \
    }                                                                                              \
    LogicalResult verify();                                                                        \
  };
ISAC_OP(AllocOp, "isac.alloc", OpTrait::VariadicOperands, OpTrait::OneResult)
ISAC_OP(LoadOp, "isac.load", OpTrait::OneOperand, OpTrait::OneResult)
ISAC_OP(StoreOp, "isac.store", OpTrait::NOperands<2>::Impl, OpTrait::ZeroResults)
ISAC_OP(CallOp, "isac.call", OpTrait::VariadicOperands, OpTrait::VariadicResults)
ISAC_OP(MemcpyOp, "isac.memcpy", OpTrait::NOperands<3>::Impl, OpTrait::OneResult)
ISAC_OP(MemmoveOp, "isac.memmove", OpTrait::NOperands<3>::Impl, OpTrait::OneResult)
ISAC_OP(FreeOp, "isac.free", OpTrait::OneOperand, OpTrait::ZeroResults)
ISAC_OP(ReturnOp, "isac.return", OpTrait::VariadicOperands, OpTrait::ZeroResults,
        OpTrait::IsTerminator)
#undef ISAC_OP
static LogicalResult offset(Operation *op, Type t) {
  if (!buffer(t))
    return op->emitOpError("requires a static rank-one i32 buffer");
  auto a = op->getAttrOfType<IntegerAttr>("offset");
  if (!a || a.getInt() < 0 || a.getInt() >= cast<MemRefType>(t).getDimSize(0))
    return op->emitOpError("invalid buffer offset");
  return success();
}
LogicalResult AllocOp::verify() {
  auto t = (*this)->getResult(0).getType();
  if (isa<LLVM::LLVMPointerType>(t))
    return getNumOperands() == 1 &&
                   (getOperand(0).getType().isInteger(32) || getOperand(0).getType().isInteger(64))
               ? success()
               : emitOpError("heap allocation needs a usize byte count");
  return buffer(t) && getNumOperands() == 0 ? success() : emitOpError("invalid stack allocation");
}
LogicalResult LoadOp::verify() {
  auto t = getOperand().getType();
  auto result = (*this)->getResult(0).getType();
  if (isa<LLVM::LLVMPointerType>(t))
    return result.isInteger(32) ? success() : emitOpError("raw load needs i32 result");
  if (!buffer(t) || cast<MemRefType>(t).getElementType() != result)
    return emitOpError("load element type mismatch");
  return offset(*this, t);
}
LogicalResult StoreOp::verify() {
  auto t = getOperand(0).getType();
  auto value = getOperand(1).getType();
  if (isa<LLVM::LLVMPointerType>(t))
    return value.isInteger(32) ? success() : emitOpError("raw store needs i32 value");
  if (!buffer(t) || cast<MemRefType>(t).getElementType() != value)
    return emitOpError("store element type mismatch");
  return offset(*this, t);
}
LogicalResult FreeOp::verify() {
  return isa<LLVM::LLVMPointerType>(getOperand().getType())
             ? success()
             : emitOpError("free requires RawPtr runtime representation");
}
static LogicalResult verifyCopy(Operation *op) {
  if (!isa<LLVM::LLVMPointerType>(op->getOperand(0).getType()) ||
      op->getOperand(1).getType() != op->getOperand(0).getType() ||
      !(op->getOperand(2).getType().isInteger(32) || op->getOperand(2).getType().isInteger(64)) ||
      op->getResult(0).getType() != op->getOperand(0).getType())
    return op->emitOpError("expected (ptr,ptr,usize) -> ptr");
  return success();
}
LogicalResult MemcpyOp::verify() {
  return verifyCopy(*this);
}
LogicalResult MemmoveOp::verify() {
  return verifyCopy(*this);
}
LogicalResult CallOp::verify() {
  auto ref = (*this)->getAttrOfType<FlatSymbolRefAttr>("callee");
  if (!ref)
    return emitOpError("missing callee");
  auto f = SymbolTable::lookupNearestSymbolFrom<func::FuncOp>(*this, ref);
  if (!f || f.getFunctionType().getInputs() != getOperandTypes() ||
      f.getFunctionType().getResults() != getResultTypes())
    return emitOpError("callee signature mismatch");
  return success();
}
LogicalResult ReturnOp::verify() {
  auto f = (*this)->getParentOfType<func::FuncOp>();
  if (!f || f.getFunctionType().getResults() != getOperandTypes())
    return emitOpError("return signature mismatch");
  return success();
}
class IsacDialect : public Dialect {
public:
  explicit IsacDialect(MLIRContext *c)
      : Dialect(getDialectNamespace(), c, TypeID::get<IsacDialect>()) {
    addOperations<AllocOp, LoadOp, StoreOp, CallOp, FreeOp, ReturnOp, MemcpyOp, MemmoveOp>();
  }
  static StringRef getDialectNamespace() {
    return "isac";
  }
};

class LowerIsacPass : public PassWrapper<LowerIsacPass, OperationPass<ModuleOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(LowerIsacPass)
  StringRef getArgument() const final {
    return "lower-isac";
  }
  void getDependentDialects(DialectRegistry &r) const override {
    r.insert<memref::MemRefDialect, func::FuncDialect, arith::ArithDialect>();
  }
  void runOnOperation() override {
    SmallVector<Operation *> ops;
    getOperation().walk([&](Operation *op) {
      if (op->getName().getDialectNamespace() == "isac")
        ops.push_back(op);
    });
    for (auto *op : ops) {
      OpBuilder b(op);
      auto loc = op->getLoc();
      Value replacement;
      if (isa<AllocOp>(op) && isa<LLVM::LLVMPointerType>(op->getResult(0).getType())) {
        replacement = b.create<func::CallOp>(
                           loc, op->hasAttr("zeroed") ? "isac_rt_alloc_zeroed" : "isac_rt_alloc",
                           op->getResultTypes(), op->getOperands())
                          .getResult(0);
      } else if (isa<FreeOp>(op))
        b.create<func::CallOp>(loc, "isac_rt_free", TypeRange{}, op->getOperands());
      else if (isa<MemcpyOp, MemmoveOp>(op))
        replacement =
            b.create<func::CallOp>(loc, isa<MemcpyOp>(op) ? "isac_rt_memcpy" : "isac_rt_memmove",
                                   op->getResultTypes(), op->getOperands())
                .getResult(0);
      else if (isa<LoadOp, StoreOp>(op) &&
               isa<LLVM::LLVMPointerType>(op->getOperand(0).getType())) {
        if (isa<LoadOp>(op))
          replacement = b.create<LLVM::LoadOp>(loc, op->getResult(0).getType(), op->getOperand(0));
        else
          b.create<LLVM::StoreOp>(loc, op->getOperand(1), op->getOperand(0));
      } else if (isa<AllocOp>(op))
        replacement = b.create<memref::AllocaOp>(loc, cast<MemRefType>(op->getResult(0).getType()));
      else if (isa<LoadOp, StoreOp>(op)) {
        auto index = b.create<arith::ConstantIndexOp>(
            loc, op->getAttrOfType<IntegerAttr>("offset").getInt());
        if (isa<LoadOp>(op))
          replacement = b.create<memref::LoadOp>(loc, op->getOperand(0), ValueRange{index});
        else
          b.create<memref::StoreOp>(loc, op->getOperand(1), op->getOperand(0), ValueRange{index});
      } else if (isa<CallOp>(op)) {
        auto call =
            b.create<func::CallOp>(loc, op->getAttrOfType<FlatSymbolRefAttr>("callee").getValue(),
                                   op->getResultTypes(), op->getOperands());
        op->replaceAllUsesWith(call.getResults());
      } else if (isa<ReturnOp>(op))
        b.create<func::ReturnOp>(loc, op->getOperands());
      else if (!isa<FreeOp>(op)) {
        op->emitError("unknown isac operation");
        signalPassFailure();
        return;
      }
      if (replacement)
        op->getResult(0).replaceAllUsesWith(replacement);
      op->erase();
    }
  }
};
}
namespace {
using namespace mlir;
class Emitter {
  MLIRContext context;
  OpBuilder builder{&context};
  OwningOpRef<ModuleOp> module;
  unsigned pointerBits = 64;
  unsigned literals = 0;
  std::unordered_map<runtime::Value, Value> values;
  Type type(runtime::Type t) {
    if (t.kind == runtime::Type::Pointer)
      return LLVM::LLVMPointerType::get(&context);
    if (t.kind == runtime::Type::Float)
      return t.elementBits == 32 ? Type(builder.getF32Type()) : Type(builder.getF64Type());
    if (t.kind == runtime::Type::Size)
      return builder.getIntegerType(t.elementBits);
    return t.kind == runtime::Type::Buffer
               ? Type(MemRefType::get({t.width}, builder.getIntegerType(t.elementBits)))
               : Type(builder.getIntegerType(t.elementBits));
  }
  Operation *op(StringRef name, TypeRange types, ValueRange operands,
                ArrayRef<NamedAttribute> attrs = {}) {
    OperationState s(builder.getUnknownLoc(), name);
    s.addTypes(types);
    s.addOperands(operands);
    s.addAttributes(attrs);
    return builder.create(s);
  }
  Value constant(uint64_t n, unsigned bits = 32) {
    return builder.create<arith::ConstantOp>(
        builder.getUnknownLoc(),
        builder.getIntegerAttr(
            builder.getIntegerType(bits),
            llvm::APInt(bits, bits == 64 ? n : n & ((uint64_t(1) << bits) - 1))));
  }
  Value truth(Value v) {
    return builder.create<arith::CmpIOp>(builder.getUnknownLoc(), arith::CmpIPredicate::ne, v,
                                         constant(0));
  }
  Value cast(Value v, Type to, bool unsign = false) {
    auto loc = builder.getUnknownLoc();
    auto from = v.getType();
    if (from == to)
      return v;
    if (isa<FloatType>(from) && isa<FloatType>(to)) {
      if (from.getIntOrFloatBitWidth() < to.getIntOrFloatBitWidth())
        return builder.create<arith::ExtFOp>(loc, to, v);
      return builder.create<arith::TruncFOp>(loc, to, v);
    }
    if (isa<FloatType>(to)) {
      if (unsign)
        return builder.create<arith::UIToFPOp>(loc, to, v);
      return builder.create<arith::SIToFPOp>(loc, to, v);
    }
    auto a = from.getIntOrFloatBitWidth(), b = to.getIntOrFloatBitWidth();
    if (a > b)
      return builder.create<arith::TruncIOp>(loc, to, v);
    if (unsign)
      return builder.create<arith::ExtUIOp>(loc, to, v);
    return builder.create<arith::ExtSIOp>(loc, to, v);
  }
  Value encode(Value v, Type cell) {
    if (isa<LLVM::LLVMPointerType>(v.getType()))
      return builder.create<LLVM::PtrToIntOp>(builder.getUnknownLoc(), cell, v);
    if (isa<FloatType>(v.getType()))
      v = builder.create<arith::BitcastOp>(
          builder.getUnknownLoc(), builder.getIntegerType(v.getType().getIntOrFloatBitWidth()), v);
    return cast(v, cell, true);
  }
  Value decode(Value v, Type to) {
    if (isa<LLVM::LLVMPointerType>(to))
      return builder.create<LLVM::IntToPtrOp>(builder.getUnknownLoc(), to, v);
    if (isa<FloatType>(to)) {
      v = cast(v, builder.getIntegerType(to.getIntOrFloatBitWidth()), true);
      return builder.create<arith::BitcastOp>(builder.getUnknownLoc(), to, v);
    }
    return cast(v, to, true);
  }
  Value binary(const std::string &name, Value a, Value b, bool unsignedOperands) {
    auto loc = builder.getUnknownLoc();
    if (isa<FloatType>(a.getType())) {
      if (name == "+")
        return builder.create<arith::AddFOp>(loc, a, b);
      if (name == "-")
        return builder.create<arith::SubFOp>(loc, a, b);
      if (name == "*")
        return builder.create<arith::MulFOp>(loc, a, b);
      if (name == "/")
        return builder.create<arith::DivFOp>(loc, a, b);
      if (name == "%")
        return builder.create<arith::RemFOp>(loc, a, b);
      auto pred = arith::CmpFPredicate::OEQ;
      if (name == "!=")
        pred = arith::CmpFPredicate::UNE;
      if (name == "<")
        pred = arith::CmpFPredicate::OLT;
      if (name == "<=")
        pred = arith::CmpFPredicate::OLE;
      if (name == ">")
        pred = arith::CmpFPredicate::OGT;
      if (name == ">=")
        pred = arith::CmpFPredicate::OGE;
      return builder.create<arith::ExtUIOp>(loc, builder.getI32Type(),
                                            builder.create<arith::CmpFOp>(loc, pred, a, b));
    }
    if (name == "^")
      return builder.create<arith::XOrIOp>(loc, a, b);
    if (name == "<<" || name == ">>") {
      auto bits = a.getType().getIntOrFloatBitWidth();
      b = builder.create<arith::AndIOp>(loc, b, constant(bits - 1, bits));
      if (name == "<<")
        return builder.create<arith::ShLIOp>(loc, a, b);
      if (unsignedOperands)
        return builder.create<arith::ShRUIOp>(loc, a, b);
      return builder.create<arith::ShRSIOp>(loc, a, b);
    }
    if (name == "+")
      return builder.create<arith::AddIOp>(loc, a, b);
    if (name == "-")
      return builder.create<arith::SubIOp>(loc, a, b);
    if (name == "*")
      return builder.create<arith::MulIOp>(loc, a, b);
    if (name == "/" || name == "%") {
      auto bits = a.getType().getIntOrFloatBitWidth();
      auto wide = builder.getI64Type();
      Value result = op("isac.call", TypeRange{wide},
                        ValueRange{cast(a, wide, unsignedOperands), cast(b, wide, unsignedOperands),
                                   constant(bits), constant(name == "%")},
                        {builder.getNamedAttr(
                            "callee", FlatSymbolRefAttr::get(&context, unsignedOperands
                                                                           ? "isac_rt_arith_u64"
                                                                           : "isac_rt_arith_i64"))})
                         ->getResult(0);
      return cast(result, a.getType(), unsignedOperands);
    }
    if (name == "&&" || name == "&")
      return builder.create<arith::AndIOp>(loc, a, b);
    if (name == "||" || name == "|")
      return builder.create<arith::OrIOp>(loc, a, b);
    arith::CmpIPredicate pred = arith::CmpIPredicate::eq;
    if (name == "!=")
      pred = arith::CmpIPredicate::ne;
    else if (name == "<")
      pred = arith::CmpIPredicate::slt;
    else if (name == ">")
      pred = arith::CmpIPredicate::sgt;
    else if (name == "<=")
      pred = arith::CmpIPredicate::sle;
    else if (name == ">=")
      pred = arith::CmpIPredicate::sge;
    if (unsignedOperands) {
      if (pred == arith::CmpIPredicate::slt)
        pred = arith::CmpIPredicate::ult;
      if (pred == arith::CmpIPredicate::sgt)
        pred = arith::CmpIPredicate::ugt;
      if (pred == arith::CmpIPredicate::sle)
        pred = arith::CmpIPredicate::ule;
      if (pred == arith::CmpIPredicate::sge)
        pred = arith::CmpIPredicate::uge;
    }
    auto cmp = builder.create<arith::CmpIOp>(loc, pred, a, b);
    return builder.create<arith::ExtUIOp>(loc, builder.getI32Type(), cmp);
  }
  void instruction(const runtime::Instruction &i) {
    using I = runtime::Instruction;
    SmallVector<Value> args;
    for (auto a : i.args)
      args.push_back(values.at(a));
    Value result;
    auto loc = builder.getUnknownLoc();
    switch (i.kind) {
    case I::RecordAlloc: {

      auto word = builder.getIntegerType(pointerBits);
      result = builder.create<LLVM::AllocaOp>(loc, LLVM::LLVMPointerType::get(&context), word,
                                              constant(i.literal, pointerBits), pointerBits / 8);
      break;
    }
    case I::StaticStr: {
      std::string name = "isac_str_" + std::to_string(literals++);
      auto ptr = LLVM::LLVMPointerType::get(&context);
      auto word = builder.getIntegerType(pointerBits);
      {
        OpBuilder::InsertionGuard guard(builder);
        builder.setInsertionPointToStart(module->getBody());
        auto bytes = LLVM::LLVMArrayType::get(builder.getI8Type(), i.callee.size());
        builder.create<LLVM::GlobalOp>(loc, bytes, true, LLVM::Linkage::Private, name,
                                       builder.getStringAttr(i.callee));
        auto record = LLVM::LLVMStructType::getLiteral(&context, {ptr, word});
        auto global = builder.create<LLVM::GlobalOp>(loc, record, true, LLVM::Linkage::Private,
                                                     name + "_view", Attribute{});
        auto *block = new Block();
        global.getInitializerRegion().push_back(block);
        builder.setInsertionPointToStart(block);
        Value v = builder.create<LLVM::UndefOp>(loc, record);
        auto address = builder.create<LLVM::AddressOfOp>(loc, ptr, name);
        v = builder.create<LLVM::InsertValueOp>(loc, v, address, ArrayRef<int64_t>{0});
        auto length = builder.create<LLVM::ConstantOp>(
            loc, word, builder.getIntegerAttr(word, i.callee.size()));
        v = builder.create<LLVM::InsertValueOp>(loc, v, length, ArrayRef<int64_t>{1});
        builder.create<LLVM::ReturnOp>(loc, v);
      }
      result = builder.create<LLVM::AddressOfOp>(loc, ptr, name + "_view");
      break;
    }
    case I::Constant:
      if (i.type.kind == runtime::Type::Float)
        result = builder.create<arith::ConstantOp>(loc, builder.getFloatAttr(type(i.type), i.real));
      else
        result = constant(i.literal, i.type.elementBits);
      break;
    case I::HeapAlloc: {
      SmallVector<NamedAttribute> attrs;
      if (i.callee == "zeroed")
        attrs.push_back(builder.getNamedAttr("zeroed", builder.getUnitAttr()));
      result = op("isac.alloc", TypeRange{type(i.type)}, args, attrs)->getResult(0);
      break;
    }
    case I::Free:
      op("isac.free", {}, args);
      break;
    case I::RawLoad:
      result = op("isac.load", TypeRange{type(i.type)}, args)->getResult(0);
      break;
    case I::RawStore:
      op("isac.store", {}, args);
      break;
    case I::MemCopy:
    case I::MemMove:
      result =
          op(i.kind == I::MemCopy ? "isac.memcpy" : "isac.memmove", TypeRange{type(i.type)}, args)
              ->getResult(0);
      break;
    case I::Alloc:
      result = op("isac.alloc", TypeRange{type(i.type)}, {})->getResult(0);
      break;
    case I::Load:
      result =
          op("isac.load", TypeRange{mlir::cast<MemRefType>(args[0].getType()).getElementType()},
             args, {builder.getNamedAttr("offset", builder.getI64IntegerAttr(i.offset))})
              ->getResult(0);
      result = decode(result, type(i.type));
      break;
    case I::Store:
      args[1] = encode(args[1], mlir::cast<MemRefType>(args[0].getType()).getElementType());
      op("isac.store", {}, args,
         {builder.getNamedAttr("offset", builder.getI64IntegerAttr(i.offset))});
      break;
    case I::Call: {
      SmallVector<Type> results;
      if (i.type.kind != runtime::Type::Void)
        results.push_back(type(i.type));
      auto *call = op("isac.call", results, args,
                      {builder.getNamedAttr("callee", FlatSymbolRefAttr::get(&context, i.callee))});
      if (!results.empty())
        result = call->getResult(0);
      break;
    }
    case I::Binary:
      result = binary(i.callee, args[0], args[1], i.unsignedOperands);
      break;
    case I::Cast:
      result = cast(args[0], type(i.type), i.unsignedOperands);
      break;
    case I::Unary:
      if (i.callee == "-")
        if (isa<FloatType>(args[0].getType()))
          result = builder.create<arith::NegFOp>(loc, args[0]);
        else
          result = builder.create<arith::SubIOp>(loc, constant(0, i.type.elementBits), args[0]);
      else
        result = builder.create<arith::XOrIOp>(loc, args[0], constant(1));
      break;
    default:
      throw std::runtime_error("structured instruction survived CFG lowering");
    }
    if (i.result != runtime::NoValue)
      values[i.result] = result;
  }

public:
  Emitter() {
    context.getOrLoadDialect<dialect::IsacDialect>();
    context.getOrLoadDialect<func::FuncDialect>();
    context.getOrLoadDialect<arith::ArithDialect>();
    context.getOrLoadDialect<cf::ControlFlowDialect>();
    context.getOrLoadDialect<memref::MemRefDialect>();
    context.getOrLoadDialect<LLVM::LLVMDialect>();
    registerBuiltinDialectTranslation(context);
    registerLLVMDialectTranslation(context);
    module = ModuleOp::create(builder.getUnknownLoc());
  }
  std::string run(const runtime::LIR &input, BackendStage stage, bool sanitize) {
    auto loc = builder.getUnknownLoc();
    (*module)->setAttr("llvm.target_triple", builder.getStringAttr(input.target.triple));
    (*module)->setAttr("llvm.data_layout", builder.getStringAttr(input.target.dataLayout));
    builder.setInsertionPointToEnd(module->getBody());
    auto ptr = LLVM::LLVMPointerType::get(&context);
    pointerBits = input.target.pointerWidth;
    auto size = builder.getIntegerType(input.target.pointerWidth);
    auto i32 = builder.getI32Type();
    auto declare = [&](StringRef name, TypeRange args, TypeRange results) {
      if (auto existing = module->lookupSymbol<func::FuncOp>(name)) {
        if (existing.getFunctionType() != builder.getFunctionType(args, results))
          throw std::runtime_error("runtime ABI signature mismatch: " + name.str());
        return existing;
      }
      auto f = builder.create<func::FuncOp>(loc, name, builder.getFunctionType(args, results));
      f.setPrivate();
      return f;
    };
    for (auto [name, bits] :
         std::vector<std::pair<std::string, unsigned>>{{"i8", 8},
                                                       {"i16", 16},
                                                       {"i32", 32},
                                                       {"i64", 64},
                                                       {"u8", 8},
                                                       {"u16", 16},
                                                       {"u32", 32},
                                                       {"u64", 64},
                                                       {"isize", input.target.pointerWidth},
                                                       {"usize", input.target.pointerWidth},
                                                       {"bool", 32},
                                                       {"char", 32}})
      declare("isac_rt_string_fmt_" + name, {ptr, builder.getIntegerType(bits)}, {});
    declare("isac_rt_string_fmt_debug_str", {ptr, ptr}, {});
    declare("isac_rt_string_fmt_debug_char", {ptr, i32}, {});
    declare("isac_rt_string_fmt_str", {ptr, ptr}, {});
    declare("isac_rt_string_fmt_f32", {ptr, builder.getF32Type()}, {});
    declare("isac_rt_string_fmt_f64", {ptr, builder.getF64Type()}, {});
    declare("isac_rt_input_parse_i32", {ptr, ptr}, {});
    declare("isac_rt_input_parse_i64", {ptr, ptr}, {});
    declare("isac_rt_input_parse_u32", {ptr, ptr}, {});
    declare("isac_rt_input_parse_u64", {ptr, ptr}, {});
    declare("isac_rt_input_parse_f32", {ptr, ptr}, {});
    declare("isac_rt_input_parse_f64", {ptr, ptr}, {});
    declare("isac_rt_input_parse_bool", {ptr, ptr}, {});
    declare("isac_rt_input_parse_view_i32", {ptr, ptr}, {});
    declare("isac_rt_input_parse_view_i64", {ptr, ptr}, {});
    declare("isac_rt_input_parse_view_u32", {ptr, ptr}, {});
    declare("isac_rt_input_parse_view_u64", {ptr, ptr}, {});
    declare("isac_rt_input_parse_view_f32", {ptr, ptr}, {});
    declare("isac_rt_input_parse_view_f64", {ptr, ptr}, {});
    declare("isac_rt_input_parse_view_bool", {ptr, ptr}, {});

    declare("isac_rt_input_read_String", {ptr}, {});
    declare("isac_rt_input_drop", {ptr}, {});
    declare("isac_rt_input_result_ok", {ptr}, {i32});
    declare("isac_rt_input_result_error", {ptr}, {i32});
    declare("isac_rt_input_result_str", {ptr, ptr}, {});
    declare("isac_rt_input_read_i32", {ptr}, {});
    declare("isac_rt_input_result_i32", {ptr}, {i32});
    declare("isac_rt_input_read_i64", {ptr}, {});
    declare("isac_rt_input_result_i64", {ptr}, {builder.getI64Type()});
    declare("isac_rt_input_read_u32", {ptr}, {});
    declare("isac_rt_input_result_u32", {ptr}, {i32});
    declare("isac_rt_input_read_u64", {ptr}, {});
    declare("isac_rt_input_result_u64", {ptr}, {builder.getI64Type()});
    declare("isac_rt_input_read_f32", {ptr}, {});
    declare("isac_rt_input_result_f32", {ptr}, {builder.getF32Type()});
    declare("isac_rt_input_read_f64", {ptr}, {});
    declare("isac_rt_input_result_f64", {ptr}, {builder.getF64Type()});
    declare("isac_rt_input_read_bool", {ptr}, {});
    declare("isac_rt_input_result_bool", {ptr}, {i32});
    declare("isac_rt_string_new", {ptr}, {});
    declare("isac_rt_string_from", {ptr, ptr}, {});
    declare("isac_rt_string_len", {ptr}, {size});
    declare("isac_rt_string_capacity", {ptr}, {size});
    declare("isac_rt_string_is_empty", {ptr}, {i32});
    declare("isac_rt_string_as_str", {ptr, ptr}, {});
    declare("isac_rt_string_push_str", {ptr, ptr}, {});
    declare("isac_rt_string_clear", {ptr}, {});
    declare("isac_rt_string_drop", {ptr}, {});
    declare("isac_rt_string_print", {ptr}, {});
    declare("isac_rt_print_view", {ptr}, {});
    auto i64 = builder.getI64Type();
    declare("isac_rt_print_i64", {i64}, {});
    declare("isac_rt_print_u64", {i64}, {});
    declare("isac_rt_print_f64", {builder.getF64Type()}, {});
    declare("isac_rt_arith_i64", {i64, i64, i32, i32}, {i64});
    declare("isac_rt_arith_u64", {i64, i64, i32, i32}, {i64});
    declare("isac_rt_print_i32", {i32}, {});
    declare("isac_rt_print_usize", {size}, {});
    declare("isac_rt_div_i32", {i32, i32}, {i32});
    declare("isac_rt_div_usize", {size, size}, {size});
    declare("isac_rt_alloc", {size}, {ptr});
    declare("isac_rt_alloc_zeroed", {size}, {ptr});
    declare("isac_rt_free", {ptr}, {});
    std::vector<func::FuncOp> funcs;
    for (const auto &f : input.functions) {
      SmallVector<Type> args, results;
      for (const auto &p : f.params)
        args.push_back(type(p.type));
      if (f.result.kind != runtime::Type::Void)
        results.push_back(type(f.result));
      auto mf = declare(f.name, args, results);
      if (!f.external)
        mf.setPublic();
      funcs.push_back(mf);
    }
    for (size_t n = 0; n < input.functions.size(); ++n) {
      auto &f = input.functions[n];
      if (f.external)
        continue;
      auto mf = funcs[n];
      values.clear();
      std::vector<Block *> blocks;
      blocks.push_back(mf.addEntryBlock());
      for (size_t j = 1; j < f.blocks.size(); ++j) {
        auto *b = new Block();
        mf.getBody().push_back(b);
        blocks.push_back(b);
      }
      for (size_t j = 0; j < f.params.size(); ++j)
        values[f.params[j].value] = blocks[0]->getArgument(j);

      for (auto &b : f.blocks) {
        builder.setInsertionPointToEnd(blocks[b.id]);
        for (auto &i : b.instructions)
          instruction(i);
        auto &t = b.end;
        if (t.kind == runtime::Terminator::Unreachable)
          builder.create<LLVM::UnreachableOp>(loc);
        else if (t.kind == runtime::Terminator::Return) {
          SmallVector<Value> args;
          if (t.value != runtime::NoValue)
            args.push_back(values.at(t.value));
          op("isac.return", {}, args);
        } else if (t.kind == runtime::Terminator::Jump)
          builder.create<cf::BranchOp>(loc, blocks[t.first]);
        else
          builder.create<cf::CondBranchOp>(loc, truth(values.at(t.value)), blocks[t.first],
                                           ValueRange{}, blocks[t.second], ValueRange{});
      }
    }
    if (failed(verify(*module)))
      throw std::runtime_error("Isac MLIR verification failed");
    if (stage != BackendStage::IsacMLIR) {
      PassManager pm(&context);
      pm.addPass(std::make_unique<dialect::LowerIsacPass>());
      if (failed(pm.run(*module)))
        throw std::runtime_error("Isac lowering failed");
    }
    if (stage == BackendStage::LLVMDialect || stage == BackendStage::LLVMIR) {
      PassManager pm(&context);
      ArithToLLVMConversionPassOptions arithmeticOptions;
      arithmeticOptions.indexBitwidth = input.target.pointerWidth;
      pm.addPass(createArithToLLVMConversionPass(arithmeticOptions));
      FinalizeMemRefToLLVMConversionPassOptions memoryOptions;
      memoryOptions.indexBitwidth = input.target.pointerWidth;
      pm.addPass(createFinalizeMemRefToLLVMConversionPass(memoryOptions));
      ConvertControlFlowToLLVMPassOptions flowOptions;
      flowOptions.indexBitwidth = input.target.pointerWidth;
      pm.addPass(createConvertControlFlowToLLVMPass(flowOptions));
      ConvertFuncToLLVMPassOptions functionOptions;
      functionOptions.indexBitwidth = input.target.pointerWidth;
      pm.addPass(createConvertFuncToLLVMPass(functionOptions));
      pm.addPass(createReconcileUnrealizedCastsPass());
      if (failed(pm.run(*module)))
        throw std::runtime_error("LLVM dialect lowering failed");
    }
    std::string output;
    llvm::raw_string_ostream stream(output);
    if (stage == BackendStage::LLVMIR) {
      llvm::LLVMContext llvmContext;
      auto llvmModule = translateModuleToLLVMIR(*module, llvmContext);
      if (!llvmModule || llvm::verifyModule(*llvmModule, &llvm::errs()))
        throw std::runtime_error("LLVM IR verification failed");
      if (sanitize)
        for (auto &function : *llvmModule)
          if (!function.isDeclaration())
            function.addFnAttr(llvm::Attribute::SanitizeAddress);
      llvmModule->print(stream, nullptr);
    } else
      module->print(stream);
    stream << "\n";
    return output;
  }
};
}
std::string generate(const runtime::LIR &p, BackendStage stage, bool sanitize) {
  return Emitter().run(p, stage, sanitize);
}
}
