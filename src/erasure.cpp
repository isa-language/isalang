#include "isac/erasure.hpp"
#include <unordered_map>
namespace isac {
namespace {
class Eraser {
  const core::Program &core;
  runtime::Erased p;
  std::unordered_map<ValueId, runtime::Value> values;
  std::unordered_map<SymbolId, runtime::Value> slots;
  std::unordered_map<runtime::Value, runtime::Type> runtimeTypes;
  using I = runtime::Instruction;
  using T = runtime::Type;
  T type(TypeId t) {
    if (t == Unit)
      return {T::Void, 0};
    if (t == USize)
      return {T::Size, 1, TargetPointerBits};
    if (t == RawPtr || core.semantics.heap(t))
      return {T::Pointer, 1};
    return core.semantics.linear(t) ? T{T::Buffer, core.semantics.types[t].width} : T{T::Scalar, 1};
  }
  std::string name(FunctionId id) {
    auto n = core.semantics.functions[id].name;
    if (core.semantics.functions[id].external)
      return n;
    size_t pos = n.find("::");
    if (pos != std::string::npos)
      n.replace(pos, 2, "__");
    return n == "main" ? n : "isa_" + std::to_string(id) + "_" + n;
  }
  runtime::Value emit(std::vector<I> &out, I i, bool result = true) {
    if (result)
      i.result = p.values++;
    auto v = i.result;
    if (result)
      runtimeTypes[v] = i.type;
    out.push_back(std::move(i));
    return v;
  }
  runtime::Value constant(std::vector<I> &out, uint64_t n, bool size = false) {
    I i{};
    i.kind = I::Constant;
    i.type = size ? T{T::Size, 1, TargetPointerBits} : T{T::Scalar, 1};
    i.literal = n;
    return emit(out, i);
  }
  void store(std::vector<I> &out, runtime::Value buffer, runtime::Value v, uint32_t offset = 0) {
    I i{};
    i.kind = I::Store;
    i.args = {buffer, v};
    i.offset = offset;
    emit(out, i, false);
  }
  std::vector<I> block(const std::vector<core::Inst> &ops) {
    std::vector<I> out;
    for (auto &c : ops) {
      using C = core::Inst;
      I i{};
      i.type = type(c.type);
      for (auto a : c.args)
        i.args.push_back(values.at(a));

      for (auto sid : c.cleanup) {
        I free{};
        free.kind = I::Free;
        free.args = {slots.at(sid)};
        emit(out, free, false);
      }
      if (c.freeTemporary) {
        I free{};
        free.kind = I::Free;
        free.args = {i.args[0]};
        emit(out, free, false);
      }
      switch (c.kind) {
      case C::HeapAlloc: {
        auto &f = core.semantics.functions[c.function];
        if (f.intrinsic == "box.new") {
          auto value = i.args[0];
          i.kind = I::HeapAlloc;
          i.args = {constant(out, 4, true)};
          auto ptr = emit(out, i);
          values[c.result] = ptr;
          I store{};
          store.kind = I::RawStore;
          store.args = {ptr, value};
          emit(out, store, false);
        } else if (f.intrinsic == "buffer.new") {
          auto length = i.args[0];
          I size{};
          size.kind = I::Call;
          size.type = {T::Size, 1, TargetPointerBits};
          size.args = {length};
          size.callee = "isac_rt_buffer_size";
          auto bytes = emit(out, size);
          i.kind = I::HeapAlloc;
          i.args = {bytes};
          i.callee = "zeroed";
          auto ptr = emit(out, i);
          values[c.result] = ptr;
          I init{};
          init.kind = I::Call;
          init.args = {ptr, length};
          init.callee = "isac_rt_buffer_init";
          emit(out, init, false);
        } else {
          i.kind = I::HeapAlloc;
          i.callee = f.name == "isac_rt_alloc_zeroed" ? "zeroed" : "";
          values[c.result] = emit(out, i);
        }
        break;
      }
      case C::HeapFree:
        i.kind = I::Free;
        emit(out, i, false);
        break;
      case C::HeapLoad:
      case C::HeapStore:
      case C::MemCopy:
      case C::MemMove: {
        auto callee = name(c.function);
        if (callee == "isac_rt_load_i32")
          i.kind = I::RawLoad;
        else if (callee == "isac_rt_store_i32")
          i.kind = I::RawStore;
        else if (c.kind == C::MemCopy)
          i.kind = I::MemCopy;
        else if (c.kind == C::MemMove)
          i.kind = I::MemMove;
        else {
          i.kind = I::Call;
          i.callee = callee;
        }
        auto v = emit(out, i, c.type != Unit);
        if (c.type != Unit)
          values[c.result] = v;
        break;
      }
      case C::Constant:
        i.kind = I::Constant;
        i.literal =
            c.type == USize ? std::stoull(c.text) : static_cast<uint64_t>(std::stoll(c.text));
        values[c.result] = emit(out, i);
        break;
      case C::Create:
      case C::Array: {
        i.kind = I::Alloc;
        i.args.clear();
        auto v = emit(out, i);
        values[c.result] = v;
        auto zero = c.kind == C::Create ? constant(out, 0) : runtime::NoValue;
        for (uint32_t n = 0; n < i.type.width; ++n)
          store(out, v, c.kind == C::Create ? zero : values.at(c.args[n]), n);
        break;
      }
      case C::Bind:
        if (core.semantics.linear(c.type) || c.type == RawPtr)
          slots[c.slot] = i.args[0];
        else {
          I alloc{};
          alloc.kind = I::Alloc;
          alloc.type = {T::Buffer, 1, c.type == USize ? TargetPointerBits : 32};
          auto v = emit(out, alloc);
          slots[c.slot] = v;
          store(out, v, i.args[0]);
        }
        break;
      case C::Copy:
      case C::Read:
        if (c.type == RawPtr) {
          values[c.result] = slots.at(c.slot);
          break;
        }
        i.kind = I::Load;
        i.args = {slots.at(c.slot)};
        i.offset = c.offset;
        values[c.result] = emit(out, i);
        break;
      case C::Move:
        values[c.result] = slots.at(c.slot);
        break;
      case C::Write:
        store(out, slots.at(c.slot), i.args[0], c.offset);
        break;
      case C::BorrowShared:
      case C::BorrowUnique:
        slots[c.offset] = slots.at(c.slot);
        break;
      case C::Binary:
      case C::Unary:
        i.kind = c.kind == C::Binary ? I::Binary : I::Unary;
        i.callee = c.text;
        i.unsignedOperands = runtimeTypes.at(i.args[0]).kind == T::Size;
        values[c.result] = emit(out, i);
        break;
      case C::Print:
        i.kind = I::Call;
        i.callee = runtimeTypes.at(i.args[0]).kind == T::Size ? "isac_rt_print_usize"
                                                              : "isac_rt_print_i32";
        emit(out, i, false);
        break;
      case C::Call:
      case C::ProtocolTransition:
        i.kind = I::Call;
        i.callee = name(c.function);
        if (c.slot != None)
          i.args.insert(i.args.begin(), slots.at(c.slot));
        if (c.type == Unit)
          emit(out, i, false);
        else
          values[c.result] = emit(out, i);
        break;
      case C::If:
        i.kind = I::If;
        i.body = block(c.body);
        i.otherwise = block(c.otherwise);
        emit(out, i, false);
        break;
      case C::Return:
        i.kind = I::Return;
        emit(out, i, false);
        return out;
      case C::Drop:
      case C::EndBorrow:
      case C::ScopeEnd:
      case C::Discard:
        break;
      }
    }
    return out;
  }

public:
  explicit Eraser(const core::Verified &v) : core(v.get()) {}
  runtime::Erased run() {
    for (const auto &hf : core.semantics.functions)
      if (hf.external) {
        runtime::Function f;
        f.external = true;
        f.name = hf.name;
        f.result = type(hf.result);
        for (auto sid : hf.params)
          f.params.push_back({p.values++, type(core.semantics.symbols[sid].type)});
        p.functions.push_back(std::move(f));
      }
    for (auto &cf : core.functions) {
      values.clear();
      slots.clear();
      auto &hf = core.semantics.functions[cf.id];
      runtime::Function f;
      f.name = name(cf.id);
      f.result = type(hf.result);
      std::vector<I> prefix;
      for (auto sid : hf.params) {
        auto t = type(core.semantics.symbols[sid].type);
        auto v = p.values++;
        f.params.push_back({v, t});
        runtimeTypes[v] = t;
        if (t.kind == T::Buffer || t.kind == T::Pointer)
          slots[sid] = v;
        else {
          I alloc{};
          alloc.kind = I::Alloc;
          alloc.type = {T::Buffer, 1, t.kind == T::Size ? TargetPointerBits : 32};
          slots[sid] = emit(prefix, alloc);
          store(prefix, slots[sid], v);
        }
      }
      f.body = std::move(prefix);
      auto b = block(cf.body);
      f.body.insert(f.body.end(), b.begin(), b.end());
      if (f.result.kind == T::Void && (f.body.empty() || f.body.back().kind != I::Return)) {
        I ret{};
        ret.kind = I::Return;
        f.body.push_back(ret);
      }
      p.functions.push_back(std::move(f));
    }
    return std::move(p);
  }
};
}
runtime::Erased erase(const core::Verified &v) {
  return Eraser(v).run();
}
}
