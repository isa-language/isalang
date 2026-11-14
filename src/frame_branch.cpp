#include "frame_heap.hpp"
#include <functional>

namespace isac::frames::detail {
namespace {
using I = core::Inst;
struct Site {
  size_t position;
  std::vector<bool> path;
};
[[noreturn]] void unsupported(Loc loc, const char *message) {
  fail(loc, DiagnosticCode::FutureFeature, message);
}
const core::Function &definition(const core::Program &p, FunctionId id, Loc loc) {
  const auto found = std::find_if(p.functions.begin(), p.functions.end(),
                                  [&](const auto &f) { return f.id == id; });
  if (found == p.functions.end())
    fail(loc, DiagnosticCode::InvalidCore, "missing branch continuation function");
  return *found;
}
Site site(const core::Program &p, FunctionId source, FunctionId target, Loc loc) {
  if (target >= p.semantics.functions.size())
    unsupported(loc, "frame step requires a resolved ordinary call boundary");
  const auto &callee = p.semantics.functions[target];
  if (callee.kind != FunctionKind::Normal || callee.intrinsicId != IntrinsicId::None ||
      callee.external || callee.trusted)
    unsupported(loc, "frame step boundary must name an ordinary Isalang function");
  std::optional<Site> found;
  std::function<void(const std::vector<I> &, size_t, std::vector<bool>, bool)> find;
  find = [&](const auto &body, size_t top, std::vector<bool> path, bool blocked) {
    for (size_t n = 0; n < body.size(); ++n) {
      const auto &i = body[n];
      const auto position = top == SIZE_MAX ? n : top;
      if (i.kind == I::Call && i.function == target) {
        if (found)
          unsupported(i.loc, "frame step requires exactly one ordinary call boundary");
        if (blocked)
          unsupported(
              i.loc,
              "frame checkpoint under a loop, unsafe scope or condition requires an edge contract");
        found = Site{position, path};
      }
      auto then = path, other = path;
      if (i.kind == I::If) {
        then.push_back(true);
        other.push_back(false);
      }
      find(i.body, position, std::move(then), blocked || i.kind != I::If);
      find(i.otherwise, position, std::move(other), blocked || i.kind != I::If);
      find(i.condition, position, path, true);
      find(i.latch, position, path, true);
    }
  };
  find(definition(p, source, loc).body, SIZE_MAX, {}, false);
  if (!found)
    unsupported(loc, "frame step call is not in the selected function/continuation");
  return std::move(*found);
}
template <class F> void walk(std::vector<I> &body, F visit) {
  for (auto &i : body) {
    visit(i);
    walk(i.body, visit);
    walk(i.otherwise, visit);
    walk(i.condition, visit);
    walk(i.latch, visit);
  }
}
template <class F> void symbols(const I &i, F visit) {
  if (i.slot != None)
    visit(i.slot);
  for (auto id : i.locals)
    visit(id);
  if (i.kind == I::BorrowShared || i.kind == I::BorrowUnique)
    visit(i.offset);
}
bool terminal(const std::vector<I> &body) {
  for (const auto &i : body) {
    if (i.kind == I::Return)
      return true;
    if (i.kind == I::If && terminal(i.body) && terminal(i.otherwise))
      return true;
    if (i.kind == I::UnsafeScope && terminal(i.body))
      return true;
  }
  return false;
}

std::vector<I> cloneTail(core::Program &p, const core::Function &f, size_t begin, Loc loc) {
  std::vector<I> copy(f.body.begin() + begin, f.body.end());
  std::set<SymbolId> external(p.semantics.functions.at(f.id).params.begin(),
                              p.semantics.functions.at(f.id).params.end());
  std::function<void(const I &)> outside = [&](const auto &i) {
    symbols(i, [&](auto id) { external.insert(id); });
    for (const auto *body : {&i.body, &i.otherwise, &i.condition, &i.latch})
      for (const auto &child : *body)
        outside(child);
  };
  for (size_t n = 0; n < begin; ++n)
    outside(f.body[n]);
  std::set<SymbolId> locals;
  walk(copy, [&](auto &i) {
    if (i.kind == I::Bind)
      locals.insert(i.slot);
    if (i.kind == I::BorrowShared || i.kind == I::BorrowUnique) {
      if (external.contains(i.offset))
        unsupported(i.loc, "branch tail cannot redefine an external reference region");
      locals.insert(i.offset);
    }
  });
  for (auto id : external)
    locals.erase(id);
  RegionId nextRegion = 0;
  uint32_t nextLoop = 0;
  auto reserve = [&](uint32_t id, uint32_t &next) {
    if (id == None)
      return;
    if (id == None - 1)
      unsupported(loc, "branch clone identity space exhausted");
    next = std::max(next, id + 1);
  };
  for (const auto &s : p.semantics.symbols)
    reserve(s.region, nextRegion);
  for (const auto &function : p.semantics.functions) {
    reserve(function.resultRegion, nextRegion);
    for (const auto &r : function.parameterRegions)
      reserve(r.region, nextRegion);
    for (const auto &r : function.payloadParameterRegions)
      reserve(r.region, nextRegion);
  }
  for (auto &function : p.functions)
    walk(function.body, [&](const auto &i) {
      reserve(i.region, nextRegion);
      reserve(i.loop, nextLoop);
    });
  std::map<SymbolId, SymbolId> symbolIds;
  std::map<RegionId, RegionId> regions;
  std::map<uint32_t, uint32_t> loops;
  std::map<ValueId, ValueId> values;
  auto fresh = [&](auto &ids, uint32_t &id, uint32_t &next) {
    if (id == None)
      return;
    auto found = ids.find(id);
    if (found == ids.end()) {
      if (next == None)
        unsupported(loc, "branch clone identity space exhausted");
      found = ids.emplace(id, next++).first;
    }
    id = found->second;
  };
  for (auto old : locals) {
    auto s = p.semantics.symbols.at(old);
    if (p.semantics.symbols.size() >= None)
      unsupported(loc, "branch symbol identity space exhausted");
    s.id = static_cast<SymbolId>(p.semantics.symbols.size());
    s.parameter = s.receiver = false;
    s.lastUse = {};
    fresh(regions, s.region, nextRegion);
    symbolIds.emplace(old, s.id);
    p.semantics.symbols.push_back(std::move(s));
  }
  walk(copy, [&](auto &i) {
    if (i.result != None)
      fresh(values, i.result, p.values);
  });
  walk(copy, [&](auto &i) {
    auto symbol = [&](SymbolId &id) {
      if (auto found = symbolIds.find(id); found != symbolIds.end())
        id = found->second;
    };
    symbol(i.slot);
    for (auto &id : i.locals)
      symbol(id);
    if (i.kind == I::BorrowShared || i.kind == I::BorrowUnique)
      symbol(i.offset);
    for (auto &arg : i.args)
      if (auto found = values.find(arg); found != values.end())
        arg = found->second;
    fresh(regions, i.region, nextRegion);
    fresh(loops, i.loop, nextLoop);
  });
  return copy;
}
}

StepLocation stepLocation(const core::Program &p, FunctionId source, FunctionId target, Loc loc) {
  const auto found = site(p, source, target, loc);
  return {found.position, static_cast<uint32_t>(found.path.size())};
}

std::optional<PreparedBranchBoundary> prepareBranchBoundary(const core::Verified &source,
                                                            FunctionId function, FunctionId target,
                                                            Loc loc) {
  auto found = site(source.get(), function, target, loc);
  if (found.path.empty())
    return {};
  LoweredRoundTrip proposal{source.get(), None, None, {}, {}};
  auto &p = proposal.program;
  uint32_t rotations = 0;
  while (!found.path.empty()) {
    auto &body = std::find_if(p.functions.begin(), p.functions.end(), [&](const auto &f) {
                   return f.id == function;
                 })->body;
    const auto &f = definition(p, function, loc);
    auto skipTail = cloneTail(p, f, found.position + 1, loc);
    auto fork = body.at(found.position);
    if (fork.kind != I::If)
      fail(loc, DiagnosticCode::InvalidCore, "branch path lost its conditional");
    auto selected = found.path[0] ? std::move(fork.body) : std::move(fork.otherwise);
    auto skipped = found.path[0] ? std::move(fork.otherwise) : std::move(fork.body);
    skipped.insert(skipped.end(), skipTail.begin(), skipTail.end());
    if (!terminal(skipped))
      unsupported(fork.loc, "branch checkpoint requires a terminal skipped path");
    fork.body.clear();
    fork.otherwise.clear();
    (found.path[0] ? fork.otherwise : fork.body) = std::move(skipped);
    std::vector<I> normalized(body.begin(), body.begin() + found.position);
    normalized.push_back(std::move(fork));
    normalized.insert(normalized.end(), selected.begin(), selected.end());
    normalized.insert(normalized.end(), body.begin() + found.position + 1, body.end());
    body = std::move(normalized);
    ++rotations;
    found = site(p, function, target, loc);
  }
  normalizeFrameValues(proposal);
  auto checked = core::verify(std::move(proposal.program), {true});
  (void)stepBoundary(checked.get(), function, target, loc);
  return PreparedBranchBoundary{std::move(checked), std::move(proposal.valueRenaming), rotations};
}
}
