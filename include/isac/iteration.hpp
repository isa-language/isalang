#pragma once
#include <optional>
#include <string_view>
#include <utility>
namespace isac {

enum class ViewOperation {
  View,
  ViewMut,
  Elements,
  ElementsMut,
  Reverse,
  Transpose,
  ReverseAxis,
  Slice,
  PermuteAxes
};
inline std::optional<ViewOperation> findViewOperation(std::string_view name) {
  constexpr std::pair<std::string_view, ViewOperation> operations[] = {
      {"view", ViewOperation::View},
      {"view_mut", ViewOperation::ViewMut},
      {"elements", ViewOperation::Elements},
      {"elements_mut", ViewOperation::ElementsMut},
      {"rev", ViewOperation::Reverse},
      {"transpose", ViewOperation::Transpose},
      {"reverse_axis", ViewOperation::ReverseAxis},
      {"slice", ViewOperation::Slice},
      {"permute_axes", ViewOperation::PermuteAxes}};
  for (const auto &[text, operation] : operations)
    if (name == text)
      return operation;
  return std::nullopt;
}
}
