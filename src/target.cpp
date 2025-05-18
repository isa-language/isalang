#include "isac/target.hpp"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Host.h"
#include <stdexcept>
namespace isac {
namespace {
void initialize() {
  static bool done = [] {
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmPrinters();
    return true;
  }();
  (void)done;
}
std::unique_ptr<llvm::TargetMachine> machine(const TargetInfo &t, OptimizationLevel o) {
  initialize();
  std::string error;
  auto target = llvm::TargetRegistry::lookupTarget(t.triple, error);
  if (!target)
    throw std::runtime_error(error);
  llvm::TargetOptions opts;
  auto level = o == OptimizationLevel::O0   ? llvm::CodeGenOptLevel::None
               : o == OptimizationLevel::O1 ? llvm::CodeGenOptLevel::Less
               : o == OptimizationLevel::O3 ? llvm::CodeGenOptLevel::Aggressive
                                            : llvm::CodeGenOptLevel::Default;
  auto tm = std::unique_ptr<llvm::TargetMachine>(target->createTargetMachine(
      llvm::Triple(t.triple), t.cpu, t.features, opts, llvm::Reloc::PIC_, std::nullopt, level));
  if (!tm)
    throw std::runtime_error("target machine unavailable");
  return tm;
}
std::unique_ptr<llvm::Module> parse(const std::string &ir, llvm::LLVMContext &c) {
  llvm::SMDiagnostic d;
  auto m = llvm::parseAssemblyString(ir, d, c);
  if (!m)
    throw std::runtime_error("cannot parse generated LLVM IR");
  return m;
}
}
std::string hostTriple() {
  return llvm::Triple::normalize(llvm::sys::getDefaultTargetTriple());
}
TargetInfo selectTarget(const TargetOptions &options) {
  initialize();
  TargetInfo t;
  t.triple = llvm::Triple::normalize(options.triple.empty() ? hostTriple() : options.triple);
  t.cpu = options.cpu;
  t.features = options.features;
  if (t.cpu == "native") {
    if (t.triple != hostTriple())
      throw std::runtime_error("--cpu=native requires the host target triple");
    t.cpu = llvm::sys::getHostCPUName().str();
    if (t.features.empty())
      for (auto &f : llvm::sys::getHostCPUFeatures()) {
        if (!t.features.empty())
          t.features += ",";
        t.features += (f.second ? "+" : "-") + f.first().str();
      }
  }
  std::string error;
  auto target = llvm::TargetRegistry::lookupTarget(t.triple, error);
  if (!target)
    throw std::runtime_error("target unavailable in this LLVM build: " + error);
  auto sti =
      std::unique_ptr<llvm::MCSubtargetInfo>(target->createMCSubtargetInfo(t.triple, "", ""));
  if (!t.cpu.empty() && sti && !sti->isCPUStringValid(t.cpu))
    throw std::runtime_error("invalid CPU for target: " + t.cpu);
  auto tm = machine(t, OptimizationLevel::O0);
  auto dl = tm->createDataLayout();
  t.dataLayout = dl.getStringRepresentation();
  t.pointerWidth = dl.getPointerSizeInBits();
  t.pointerAlignment = dl.getPointerABIAlignment(0).value();
  t.littleEndian = dl.isLittleEndian();
  llvm::LLVMContext context;
  for (unsigned n = 0; n < 4; ++n)
    t.integerAlignment[n] = dl.getABITypeAlign(llvm::IntegerType::get(context, 8u << n)).value();
  t.floatAlignment[0] = dl.getABITypeAlign(llvm::Type::getFloatTy(context)).value();
  t.floatAlignment[1] = dl.getABITypeAlign(llvm::Type::getDoubleTy(context)).value();
  if (t.pointerWidth != 32 && t.pointerWidth != 64)
    throw std::runtime_error("PoC supports 32-bit and 64-bit pointers");
  return t;
}
std::string optimizeLLVM(const std::string &ir, const TargetInfo &t, OptimizationLevel o) {
  llvm::LLVMContext c;
  auto m = parse(ir, c);
  auto tm = machine(t, o);
  m->setTargetTriple(llvm::Triple(t.triple));
  m->setDataLayout(tm->createDataLayout());
  for (auto &f : *m)
    if (!f.isDeclaration()) {
      if (!t.cpu.empty())
        f.addFnAttr("target-cpu", t.cpu);
      if (!t.features.empty())
        f.addFnAttr("target-features", t.features);
    }
  if (o != OptimizationLevel::O0) {
    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cam;
    llvm::ModuleAnalysisManager mam;
    llvm::PassBuilder pb(tm.get());
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cam, mam);
    auto level = o == OptimizationLevel::O1   ? llvm::OptimizationLevel::O1
                 : o == OptimizationLevel::O2 ? llvm::OptimizationLevel::O2
                 : o == OptimizationLevel::O3 ? llvm::OptimizationLevel::O3
                 : o == OptimizationLevel::Os ? llvm::OptimizationLevel::Os
                                              : llvm::OptimizationLevel::Oz;
    auto pipeline = pb.buildPerModuleDefaultPipeline(level);
    pipeline.run(*m, mam);
  }
  if (llvm::verifyModule(*m, &llvm::errs()))
    throw std::runtime_error("optimized LLVM verification failed");
  std::string out;
  llvm::raw_string_ostream os(out);
  m->print(os, nullptr);
  return out;
}
void emitMachine(const std::string &ir, const std::string &output, const TargetInfo &t,
                 OptimizationLevel o, bool assembly) {
  llvm::LLVMContext c;
  auto m = parse(ir, c);
  auto tm = machine(t, o);
  std::error_code ec;
  llvm::raw_fd_ostream file(output, ec, llvm::sys::fs::OF_None);
  if (ec)
    throw std::runtime_error(ec.message());
  llvm::legacy::PassManager pm;
  if (tm->addPassesToEmitFile(pm, file, nullptr,
                              assembly ? llvm::CodeGenFileType::AssemblyFile
                                       : llvm::CodeGenFileType::ObjectFile))
    throw std::runtime_error("target cannot emit this format");
  pm.run(*m);
  file.flush();
  if (file.has_error())
    throw std::runtime_error("failed writing machine output");
}
}
