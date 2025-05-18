#pragma once
#include <array>
#include <string>
namespace isac {
enum class OptimizationLevel { O0, O1, O2, O3, Os, Oz };
enum class EmitKind { Check, IR, Assembly, Object, Executable };
struct TargetOptions {
  std::string triple, cpu, features;
};
struct TargetInfo {
  std::string triple, cpu, features, dataLayout;
  unsigned pointerWidth = 64, pointerAlignment = 8;
  bool littleEndian = true;
  std::array<unsigned, 4> integerAlignment = {1, 2, 4, 8};
  std::array<unsigned, 2> floatAlignment = {4, 8};
};
struct CompilerOptions {
  TargetOptions target;
  OptimizationLevel optimization = OptimizationLevel::O0;
  EmitKind emit = EmitKind::Check;
};
TargetInfo selectTarget(const TargetOptions &);
std::string hostTriple();
std::string optimizeLLVM(const std::string &, const TargetInfo &, OptimizationLevel);
void emitMachine(const std::string &, const std::string &, const TargetInfo &, OptimizationLevel,
                 bool assembly);
}
