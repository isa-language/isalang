#pragma once
#include "isac/hir.hpp"
#include <utility>
namespace isac::ffi {
bool validName(const std::string &);

std::pair<uint32_t, uint32_t> layout(const hir::Program &, TypeId);
enum class ParameterKind { Scalar, BorrowedScalar, Array, Slice, Text };
ParameterKind parameterKind(const hir::Program &, const hir::Symbol &);
unsigned parameterCells(const hir::Program &, const hir::Function &);
bool signature(const hir::Program &, const hir::Function &);
bool callbackSignature(const hir::Program &, TypeId, const hir::Function &);
std::string addressName(FunctionId);
std::string indirectName(TypeId);
std::string bridge(const hir::Program &);
std::string importName(FunctionId);
std::string exportName(FunctionId);
}
