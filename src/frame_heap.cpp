#include "frame_heap.hpp"
#include "isac/frame_lifecycle.hpp"
#include "isac/execution.hpp"
#include <functional>

namespace isac::frames::detail {
GenericDeclId boxDeclaration(const hir::Program &p, Loc loc) {
  for (const auto &decl : p.genericStructs)
    if (decl.representation == hir::Representation::GenericBox)
      return decl.id;
  fail(loc, DiagnosticCode::FutureFeature,
       "heap frame requires a resolved Box declaration; import std.box");
}
void validateHeapInputs(const core::Program &p, const Verified &frame, Loc loc) {
  (void)boxDeclaration(p.semantics, loc);
  auto initial = [&](TypeId type) {
    auto q = p.semantics.types.at(type).protocol;
    return q == None ? None : p.semantics.protocols.at(q).initial;
  };
  std::function<bool(TypeId, const std::map<uint32_t, core::OwnedFieldState> &)> retained =
      [&](TypeId type, const auto &fields) {
        for (auto index : p.semantics.managedFields(type)) {
          auto found = fields.find(index);
          const auto member = p.semantics.types[type].fields[index].type;
          if (found == fields.end() || found->second.state != initial(member) ||
              found->second.allocation != p.semantics.needsDrop(member) ||
              !retained(member, found->second.fields))
            return false;
        }
        return true;
      };
  for (const auto &slot : frame.get().slots) {
    const auto &value = frame.content(slot.id);
    if (!execution::closedStorage(p.semantics, slot.type) ||
        !p.semantics.containerElement(slot.type) || value.protocolState != initial(slot.type) ||
        value.allocation != p.semantics.needsDrop(slot.type) || !retained(slot.type, value.fields))
      fail(
          loc, DiagnosticCode::FutureFeature,
          "heap frame requires closed sources in their proved droppable initial state with retained allocation contracts");
  }
}
void defineHeapFactories(LoweredRoundTrip &lowered, Loc loc) {
  auto &p = lowered.program;
  auto decl = boxDeclaration(p.semantics, loc);
  lowered.heapType = hir::instantiateHeapContainer(p.semantics, decl, {lowered.frameType}, loc);
  auto boxNew =
      hir::owningBoxIntrinsic(p.semantics, lowered.heapType, IntrinsicId::Box_new_generic, loc);
  auto boxTake =
      hir::owningBoxIntrinsic(p.semantics, lowered.heapType, IntrinsicId::Box_into_inner, loc);
  auto symbol = [&](TypeId type, const std::string &name, bool parameter) {
    hir::Symbol s{};
    s.id = static_cast<SymbolId>(p.semantics.symbols.size());
    s.name = name;
    s.type = type;
    s.parameter = parameter;
    s.loc = loc;
    p.semantics.symbols.push_back(s);
    return s.id;
  };
  lowered.heapStorage = symbol(lowered.heapType, "$frame.heap", false);
  enum Role { Make, Restore, Destroy };
  auto helper = [&](Role role) {
    const auto input = role == Make ? lowered.frameType : lowered.heapType;
    const auto output = role == Make      ? lowered.heapType
                        : role == Restore ? lowered.frameType
                                          : Unit;
    hir::Function signature{};
    signature.id = static_cast<FunctionId>(p.semantics.functions.size());
    signature.name = std::string("__frame_") +
                     (role == Make      ? "make_"
                      : role == Restore ? "restore_"
                                        : "destroy_") +
                     std::to_string(signature.id);
    signature.result = output;
    signature.loc = loc;
    signature.effects.allocation = role == Make;
    auto parameter = symbol(input, "$frame.input", true);
    signature.params = {parameter};
    auto id = signature.id;
    p.semantics.functions.push_back(std::move(signature));
    core::Function function{id, {}, {p.values++}, {}};
    auto operation = [&](core::Inst::Kind kind, TypeId type) {
      core::Inst i{};
      i.kind = kind;
      i.type = type;
      i.loc = loc;
      i.quantity = p.semantics.linear(type) ? Quantity::One : Quantity::Omega;
      return i;
    };
    auto emit = [&](core::Inst i, bool produces = false) {
      if (produces)
        i.result = p.values++;
      auto value = i.result;
      function.body.push_back(std::move(i));
      return value;
    };
    ValueId result = None;
    if (role == Restore) {
      auto call = operation(core::Inst::ContainerCall, lowered.frameType);
      call.function = boxTake;
      call.slot = parameter;
      call.access = Access::Own;
      call.intrinsicId = IntrinsicId::Box_into_inner;
      call.text = std::string(intrinsicInfo(call.intrinsicId).name);
      result = emit(call, true);
    } else {
      auto move = operation(core::Inst::Move, input);
      move.slot = parameter;
      auto value = emit(move, true);
      if (role == Make) {
        auto call = operation(core::Inst::ContainerCall, lowered.heapType);
        call.function = boxNew;
        call.intrinsicId = IntrinsicId::Box_new_generic;
        call.text = std::string(intrinsicInfo(call.intrinsicId).name);
        call.args = {value};
        result = emit(call, true);
      } else {
        auto drop = operation(core::Inst::Discard, input);
        drop.args = {value};
        emit(drop);
      }
    }
    auto ret = operation(core::Inst::Return, output);
    if (result != None)
      ret.args = {result};
    emit(ret);
    p.functions.push_back(std::move(function));
    return id;
  };
  lowered.factory = helper(Make);
  lowered.restore = helper(Restore);
  lowered.destroy = helper(Destroy);
}
std::vector<core::Inst> lowerHeapLifecycle(LoweredRoundTrip &lowered, const Verified &frame,
                                           Loc loc, TypeId functionResult, bool destroy,
                                           std::optional<ValueId> selector) {
  auto &p = lowered.program;
  using I = core::Inst;
  using O = LifecycleOp;

  LifecycleGraph graph;
  if (selector)
    graph.blocks = {{{}, {1, 2}, loc},
                    {{O::Destroy, O::Finish}, {}, loc},
                    {{O::Restore, O::Deliver, O::Finish}, {}, loc}};
  else
    graph.blocks = {{destroy ? std::vector{O::Destroy, O::Finish}
                             : std::vector{O::Restore, O::Deliver, O::Finish},
                     {},
                     loc}};
  auto checked = verifyLifecycleGraph(frame, std::move(graph));
  auto operation = [&](I::Kind kind, TypeId type) {
    I i{};
    i.kind = kind;
    i.type = type;
    i.loc = loc;
    i.quantity = p.semantics.linear(type) ? Quantity::One : Quantity::Omega;
    return i;
  };
  auto path = [&](LifecycleBlockId block, bool exits) {
    std::vector<I> body;
    auto emit = [&](I i, bool produces = false) {
      if (produces)
        i.result = p.values++;
      auto value = i.result;
      body.push_back(std::move(i));
      return value;
    };
    auto move = operation(I::Move, lowered.heapType);
    move.slot = lowered.heapStorage;
    auto owned = emit(move, true);
    ValueId restored = None;
    for (auto op : checked.get().blocks.at(block).operations)
      switch (op) {
      case O::Restore:
      case O::Destroy: {
        const bool destroys = op == O::Destroy;
        auto call = operation(I::Call, destroys ? Unit : lowered.frameType);
        call.function = destroys ? lowered.destroy : lowered.restore;
        call.args = {owned};
        restored = emit(call, !destroys);
        break;
      }
      case O::Deliver: {
        auto bind = operation(I::Bind, lowered.frameType);
        bind.slot = lowered.storage;
        bind.args = {restored};
        emit(bind);
        break;
      }
      case O::Finish: {
        if (exits) {
          ValueId code = None;
          if (functionResult == I32) {
            auto zero = operation(I::Constant, I32);
            zero.text = "0";
            code = emit(zero, true);
          }
          auto ret = operation(I::Return, functionResult);
          if (code != None)
            ret.args = {code};
          emit(ret);
        }
        break;
      }
      }
    return body;
  };
  if (!selector)
    return path(checked.get().entry, false);
  auto branch = operation(I::If, Unit);
  branch.args = {*selector};
  const auto &edges = checked.get().blocks.at(checked.get().entry).successors;
  branch.body = path(edges.at(0), true);
  branch.otherwise = path(edges.at(1), false);
  return {std::move(branch)};
}
}
