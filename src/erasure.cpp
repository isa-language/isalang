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
      return {T::Size, 1, core.semantics.target.pointerWidth, false};
    if (t == RawPtr || t == Str || core.semantics.heap(t))
      return {T::Pointer, 1};
    return core.semantics.linear(t)
               ? T{T::Buffer, core.semantics.types[t].width, 64}
               : T{floating(t) ? T::Float : T::Scalar, 1,
                   bitWidth(t, core.semantics.target.pointerWidth), signedType(t)};
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
    i.type = size ? T{T::Size, 1, core.semantics.target.pointerWidth} : T{T::Scalar, 1};
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
      if (c.kind == core::Inst::Loop)
        i.condition = block(c.condition);
      for (auto a : c.args)
        i.args.push_back(values.at(a));

      for (auto sid : c.cleanup) {
        I free{};
        free.kind = I::Free;
        if (core.semantics.types[core.semantics.symbols[sid].type].representation ==
            hir::Representation::StringRecord) {
          free.kind = I::Call;
          free.callee = "isac_rt_string_drop";
        }
        if (core.semantics.types[core.semantics.symbols[sid].type].representation ==
            hir::Representation::InputRecord) {
          free.kind = I::Call;
          free.callee = "isac_rt_input_drop";
        }
        free.args = {slots.at(sid)};
        emit(out, free, false);
      }
      if (c.freeTemporary) {
        I free{};
        free.kind = I::Free;
        if (core.semantics.types[c.type].representation == hir::Representation::StringRecord) {
          free.kind = I::Call;
          free.callee = "isac_rt_string_drop";
        }
        if (core.semantics.types[c.type].representation == hir::Representation::InputRecord) {
          free.kind = I::Call;
          free.callee = "isac_rt_input_drop";
        }
        free.args = {i.args[0]};
        emit(out, free, false);
      }
      switch (c.kind) {
      case C::StaticStr:
        i.kind = I::StaticStr;
        i.callee = c.text;
        values[c.result] = emit(out, i);
        break;
      case C::TextPrint:
        i.kind = I::Call;
        i.callee = "isac_rt_string_print";
        i.args = {slots.at(c.slot)};
        emit(out, i, false);
        break;
      case C::InputCall:
      case C::StringCall: {
        i.kind = I::Call;
        i.callee = intrinsicRuntimeSymbol(c.intrinsicId);
        if (c.slot != None)
          i.args.insert(i.args.begin(), slots.at(c.slot));
        bool descriptor = c.text == "string.new" || c.text == "string.from" ||
                          c.text == "string.as_str" || c.text.starts_with("io.read_") ||
                          c.text.starts_with("io.parse_") ||
                          c.intrinsicId == IntrinsicId::Input_result_str;
        if (descriptor) {
          I alloc{};
          alloc.kind = I::RecordAlloc;
          alloc.type = {T::Pointer, 1};
          alloc.literal = c.type == Str ? 2 : c.kind == C::InputCall ? 6 : 3;
          auto ptr = emit(out, alloc);
          values[c.result] = ptr;
          i.args.insert(i.args.begin(), ptr);
          i.type = {T::Void, 0};
          emit(out, i, false);
        } else {
          auto v = emit(out, i, c.type != Unit);
          if (c.type != Unit)
            values[c.result] = v;
        }
        break;
      }
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
          size.type = {T::Size, 1, core.semantics.target.pointerWidth};
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
        if (floating(c.type))
          i.real = std::stod(c.text);
        else
          i.literal =
              signedType(c.type) ? static_cast<uint64_t>(std::stoll(c.text)) : std::stoull(c.text);
        values[c.result] = emit(out, i);
        break;
      case C::EnumConstruct: {
        i.kind = I::Alloc;
        i.args.clear();
        auto v = emit(out, i);
        values[c.result] = v;
        store(out, v, constant(out, c.offset), 0);
        for (size_t n = 0; n < c.args.size(); ++n)
          store(out, v, values.at(c.args[n]), n + 1);
        break;
      }
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
        if (core.semantics.linear(c.type) || c.type == RawPtr || c.type == Str)
          slots[c.slot] = i.args[0];
        else {
          I alloc{};
          alloc.kind = I::Alloc;
          alloc.type = {T::Buffer, 1, 64};
          auto v = emit(out, alloc);
          slots[c.slot] = v;
          store(out, v, i.args[0]);
        }
        break;
      case C::Copy:
      case C::Read:
        if (c.type == RawPtr || c.type == Str) {
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
      case C::Cast:
      case C::Binary:
      case C::Unary:
        i.kind = c.kind == C::Cast ? I::Cast : c.kind == C::Binary ? I::Binary : I::Unary;
        i.callee = c.text;
        i.unsignedOperands = !runtimeTypes.at(i.args[0]).isSigned;
        values[c.result] = emit(out, i);
        break;
      case C::Print:
        i.kind = I::Call;
        {
          auto t = runtimeTypes.at(i.args[0]);
          if (t.kind == T::Pointer) {
            i.callee = "isac_rt_print_view";
            emit(out, i, false);
            break;
          }
          if (t.kind == T::Scalar && t.elementBits == 32 && t.isSigned)
            i.callee = "isac_rt_print_i32";
          else if (t.kind == T::Size)
            i.callee = "isac_rt_print_usize";
          else {
            I cast{};
            cast.kind = I::Cast;
            cast.args = i.args;
            cast.unsignedOperands = !t.isSigned;
            cast.type = {t.kind == T::Float ? T::Float : T::Scalar, 1, 64, t.isSigned};
            i.args = {emit(out, cast)};
            i.callee = t.kind == T::Float ? "isac_rt_print_f64"
                       : t.isSigned       ? "isac_rt_print_i64"
                                          : "isac_rt_print_u64";
          }
        }
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
      case C::Loop:
        i.kind = I::Loop;
        i.loop = c.loop;
        i.unconditional = c.unconditional;
        i.body = block(c.body);
        i.latch = block(c.latch);
        emit(out, i, false);
        break;
      case C::Break:
      case C::Continue:
        i.kind = c.kind == C::Break ? I::Break : I::Continue;
        i.loop = c.loop;
        emit(out, i, false);
        return out;
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
      case C::EndViews:
      case C::Inspect:
      case C::RefineVariant:
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
    p.target = core.semantics.target;
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
        if (core.semantics.symbols[sid].receiver && scalar(core.semantics.symbols[sid].type))
          t = {T::Buffer, 1, 64};
        auto v = p.values++;
        f.params.push_back({v, t});
        runtimeTypes[v] = t;
        if (t.kind == T::Buffer || t.kind == T::Pointer)
          slots[sid] = v;
        else {
          I alloc{};
          alloc.kind = I::Alloc;
          alloc.type = {T::Buffer, 1, 64};
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
