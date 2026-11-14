#include "frame_heap.hpp"
#include <functional>

namespace isac::frames::detail {
namespace {
using I = core::Inst;

template <class F> void symbols(I &i, F visit) {
  if (i.slot != None)
    visit(i.slot);
  for (auto &local : i.locals)
    visit(local);
  if (i.kind == I::BorrowShared || i.kind == I::BorrowUnique)
    visit(i.offset);
}
template <class F> void walk(std::vector<I> &body, F visit) {
  for (auto &i : body) {
    visit(i);
    walk(i.condition, visit);
    walk(i.body, visit);
    walk(i.otherwise, visit);
    walk(i.latch, visit);
  }
}
}

void outlineContinuation(LoweredRoundTrip &lowered, FunctionId source, Loc loc,
                         std::vector<I> &prefix, std::vector<I> suffix, bool dispatch) {
  auto &p = lowered.program;
  const auto resultType = p.semantics.functions.at(source).result;
  size_t cut = prefix.size();
  for (size_t n = 0; n < prefix.size(); ++n)
    if (prefix[n].kind == I::Bind && prefix[n].slot == lowered.heapStorage) {
      cut = n + 1;
      break;
    }
  if (cut == prefix.size())
    fail(loc, DiagnosticCode::InvalidCore, "missing sealed heap frame for continuation extraction");
  std::vector<I> body;
  if (dispatch) {
    if (prefix.at(cut).kind != I::If)
      fail(loc, DiagnosticCode::InvalidCore, "missing lifecycle fork for continuation extraction");
    body = std::move(prefix[cut].otherwise);
    prefix[cut].otherwise.clear();
    body.insert(body.end(), prefix.begin() + cut + 1, prefix.end());
    prefix.resize(cut + 1);
  } else {
    body.assign(prefix.begin() + cut, prefix.end());
    prefix.resize(cut);
  }
  body.insert(body.end(), suffix.begin(), suffix.end());

  std::set<SymbolId> locals;
  walk(body, [&](I &i) { symbols(i, [&](SymbolId &id) { locals.insert(id); }); });
  if (!locals.contains(lowered.heapStorage))
    fail(loc, DiagnosticCode::InvalidCore, "outlined continuation has no owned frame input");
  RegionId nextRegion = 0;
  auto reserve = [&](RegionId region) {
    if (region != None) {
      if (region == None - 1)
        fail(loc, DiagnosticCode::InvalidCore, "region identity space exhausted");
      nextRegion = std::max(nextRegion, region + 1);
    }
  };
  for (const auto &s : p.semantics.symbols)
    reserve(s.region);
  for (const auto &f : p.semantics.functions) {
    reserve(f.resultRegion);
    for (const auto &input : f.parameterRegions)
      reserve(input.region);
    for (const auto &input : f.payloadParameterRegions)
      reserve(input.region);
  }
  for (auto &f : p.functions)
    walk(f.body, [&](I &i) { reserve(i.region); });
  std::map<RegionId, RegionId> regionIds;
  auto region = [&](RegionId &id) {
    if (id == None)
      return;
    auto found = regionIds.find(id);
    if (found == regionIds.end()) {
      if (nextRegion == None)
        fail(loc, DiagnosticCode::InvalidCore, "region identity space exhausted");
      found = regionIds.emplace(id, nextRegion++).first;
    }
    id = found->second;
  };
  for (auto old : locals) {
    auto symbol = p.semantics.symbols.at(old);
    symbol.id = static_cast<SymbolId>(p.semantics.symbols.size());
    symbol.parameter = old == lowered.heapStorage;
    symbol.receiver = false;
    region(symbol.region);
    symbol.lastUse = {};
    lowered.continuationSymbols.emplace(old, symbol.id);
    p.semantics.symbols.push_back(std::move(symbol));
  }
  walk(body, [&](I &i) {
    symbols(i, [&](SymbolId &id) { id = lowered.continuationSymbols.at(id); });
    region(i.region);
  });
  for (auto &[old, restored] : lowered.restoredSymbols) {
    (void)old;
    restored = lowered.continuationSymbols.at(restored);
  }
  hir::Function signature{};
  signature.id = static_cast<FunctionId>(p.semantics.functions.size());
  signature.name = "__frame_continue_" + std::to_string(signature.id);
  signature.params = {lowered.continuationSymbols.at(lowered.heapStorage)};
  signature.result = resultType;
  signature.loc = loc;
  signature.effects = p.semantics.functions.at(source).effects;
  signature.unsafeFunction = p.semantics.functions.at(source).unsafeFunction;
  lowered.continuation = signature.id;
  p.semantics.functions.push_back(std::move(signature));
  p.functions.push_back({lowered.continuation, std::move(body), {p.values++}, {}});

  auto &callSite = dispatch ? prefix.at(cut).otherwise : prefix;
  auto operation = [&](I::Kind kind, TypeId type) {
    I i{};
    i.kind = kind;
    i.type = type;
    i.loc = loc;
    i.quantity = p.semantics.linear(type) ? Quantity::One : Quantity::Omega;
    return i;
  };
  auto move = operation(I::Move, lowered.heapType);
  move.slot = lowered.heapStorage;
  move.result = p.values++;
  auto call = operation(I::Call, resultType);
  call.function = lowered.continuation;
  call.args = {move.result};
  if (resultType != Unit)
    call.result = p.values++;
  auto ret = operation(I::Return, resultType);
  if (call.result != None)
    ret.args = {call.result};
  callSite.insert(callSite.end(), {move, call, ret});
}
}
