#pragma once
#include "isac/runtime.hpp"
namespace isac {
enum class BackendStage { IsacMLIR, StandardMLIR, LLVMDialect, LLVMIR };
struct NativeOptions {
  bool sanitize = false, counters = false;
  bool threads = false;
  std::vector<std::string> linkInputs, libraryPaths, libraries;
  TargetInfo target;
  OptimizationLevel optimization = OptimizationLevel::O0;
};
std::string compileCBridge(const std::string &, const TargetInfo &, bool sanitize = false);
std::string generate(const runtime::LIR &, BackendStage, bool sanitize = false);
void compileNative(const std::string &llvmIR, const std::string &output,
                   NativeOptions options = {});
}
