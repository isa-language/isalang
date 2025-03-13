#include "isac/backend.hpp"
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
    return getNumOperands() == 1 && getOperand(0).getType().isInteger(64)
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
      !op->getOperand(2).getType().isInteger(64) ||
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
  std::unordered_map<runtime::Value, Value> values;
  Type type(runtime::Type t) {
    if (t.kind == runtime::Type::Pointer)
      return LLVM::LLVMPointerType::get(&context);
    if (t.kind == runtime::Type::Size)
      return builder.getIntegerType(t.elementBits);
    return t.kind == runtime::Type::Buffer
               ? Type(MemRefType::get({t.width}, builder.getIntegerType(t.elementBits)))
               : Type(builder.getI32Type());
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
        builder.getIntegerAttr(builder.getIntegerType(bits),
                               llvm::APInt(bits, bits == 32 ? static_cast<uint32_t>(n) : n)));
  }
  Value truth(Value v) {
    return builder.create<arith::CmpIOp>(builder.getUnknownLoc(), arith::CmpIPredicate::ne, v,
                                         constant(0));
  }
  Value binary(const std::string &name, Value a, Value b) {
    auto loc = builder.getUnknownLoc();
    if (name == "+")
      return builder.create<arith::AddIOp>(loc, a, b);
    if (name == "-")
      return builder.create<arith::SubIOp>(loc, a, b);
    if (name == "*")
      return builder.create<arith::MulIOp>(loc, a, b);
    if (name == "/")
      return op("isac.call", TypeRange{a.getType()}, ValueRange{a, b},
                {builder.getNamedAttr("callee",
                                      FlatSymbolRefAttr::get(&context, a.getType().isInteger(64)
                                                                           ? "isac_rt_div_usize"
                                                                           : "isac_rt_div_i32"))})
          ->getResult(0);
    if (name == "&&")
      return builder.create<arith::AndIOp>(loc, a, b);
    if (name == "||")
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
    if (a.getType().isInteger(64)) {
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
    case I::Constant:
      result = constant(i.literal, i.type.kind == runtime::Type::Size ? i.type.elementBits : 32);
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
      result = op("isac.load", TypeRange{type(i.type)}, args,
                  {builder.getNamedAttr("offset", builder.getI64IntegerAttr(i.offset))})
                   ->getResult(0);
      break;
    case I::Store:
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
      result = binary(i.callee, args[0], args[1]);
      break;
    case I::Unary:
      if (i.callee == "-")
        result = builder.create<arith::SubIOp>(loc, constant(0), args[0]);
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
    builder.setInsertionPointToEnd(module->getBody());
    auto ptr = LLVM::LLVMPointerType::get(&context);
    auto size = builder.getI64Type();
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
        if (t.kind == runtime::Terminator::Return) {
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
      pm.addPass(createArithToLLVMConversionPass());
      pm.addPass(createFinalizeMemRefToLLVMConversionPass());
      pm.addPass(createConvertControlFlowToLLVMPass());
      pm.addPass(createConvertFuncToLLVMPass());
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
