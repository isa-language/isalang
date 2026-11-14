#pragma once
#include "isac/frames.hpp"
namespace isac::frames::detail {
inline constexpr VariantId SegmentContinue = 0, SegmentComplete = 1;
GenericDeclId boxDeclaration(const hir::Program &, Loc);
void validateHeapInputs(const core::Program &, const Verified &, Loc);
void defineHeapFactories(LoweredRoundTrip &, Loc);

std::vector<core::Inst> lowerHeapLifecycle(LoweredRoundTrip &, const Verified &, Loc,
                                           TypeId functionResult, bool destroy,
                                           std::optional<ValueId> selector);

void outlineContinuation(LoweredRoundTrip &, FunctionId source, Loc,
                         std::vector<core::Inst> &prefix, std::vector<core::Inst> suffix,
                         bool dispatch);
void materializeProgress(LoweredRoundTrip &, FunctionId source, Loc,
                         std::vector<core::Inst> &prefix, RoundTripOptions);
ProgressStorage defineProgressStorage(core::Program &, TypeId result,
                                      const std::vector<TypeId> &initialTypes, FunctionId initial,
                                      const std::vector<ProgressStep> &, Loc);

void splitContinuation(LoweredRoundTrip &, FunctionId stepBefore, Loc, bool projectFields = false,
                       FunctionId source = None);

size_t stepBoundary(const core::Program &, FunctionId source, FunctionId target, Loc);
struct StepLocation {
  size_t position;
  uint32_t branches;
};
StepLocation stepLocation(const core::Program &, FunctionId source, FunctionId target, Loc);
struct PreparedBranchBoundary {
  core::Verified source;
  std::map<ValueId, ValueId> valueRenaming;
  uint32_t rotations;
};

std::optional<PreparedBranchBoundary> prepareBranchBoundary(const core::Verified &, FunctionId,
                                                            FunctionId, Loc);
std::optional<size_t> stepOrder(const core::Program &, FunctionId source, RoundTripOptions, Loc);
size_t capturePosition(const core::Verified &, const core::CaptureState &);
void normalizeFrameValues(LoweredRoundTrip &);
}
