#include "isac/erasure.hpp"
#include <algorithm>
#include <unordered_map>
namespace isac {
namespace {
class Eraser {
  const core::Program &core;
  runtime::Erased p;
  std::unordered_map<ValueId, runtime::Value> values;
  std::unordered_map<SymbolId, runtime::Value> slots;
  std::unordered_map<runtime::Value, runtime::Type> runtimeTypes;
  runtime::Value returnStorage = runtime::NoValue;
  TypeId returnType = Unit;
  using I = runtime::Instruction;
  using T = runtime::Type;
  T type(TypeId t) {
    if (t == Unit)
      return {T::Void, 0};
    if (t == USize)
      return {T::Size, 1, core.semantics.target.pointerWidth, false};
    if (t == RawPtr || t == Str || core.semantics.heap(t) || core.semantics.reference(t))
      return {T::Pointer, 1};
    return core.semantics.linear(t)
               ? T{T::Buffer, core.semantics.types[t].width, 64}
               : T{floating(t) ? T::Float : T::Scalar, 1,
                   bitWidth(t, core.semantics.target.pointerWidth), signedType(t)};
  }
  T referenceType(TypeId t) {
    return scalar(t) ? T{T::Buffer, 1, 64, true, true} : type(t);
  }
  runtime::Value referenceView(std::vector<I> &out, runtime::Value value, TypeId t) {
    auto target = referenceType(t);
    if (runtimeTypes.at(value) == target)
      return value;
    I view{};
    view.kind = I::View;
    view.type = target;
    view.args = {value};
    return emit(out, view);
  }
  unsigned recordWords(TypeId tid) const {
    auto representation = core.semantics.types[tid].representation;
    return representation == hir::Representation::StringRecord  ? 3
           : representation == hir::Representation::InputRecord ? 6
                                                                : 0;
  }
  std::string name(FunctionId id) {
    auto n = core.semantics.functions[id].name;
    if (core.semantics.functions[id].external)
      return n;
    size_t pos = n.find("::");
    if (pos != std::string::npos)
      n.replace(pos, 2, "__");
    return "isa_" + std::to_string(id) + "_" + n;
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
  runtime::Value load(std::vector<I> &out, runtime::Value buffer, uint32_t offset, T t) {
    I i{};
    i.kind = I::Load;
    i.type = t;
    i.args = {buffer};
    i.offset = offset;
    return emit(out, i);
  }
  runtime::Value project(std::vector<I> &out, runtime::Value buffer, uint32_t offset, TypeId tid) {
    auto t = type(tid);
    if (t.kind != T::Buffer)
      return load(out, buffer, offset, t);
    I alloc{};
    alloc.kind = I::Alloc;
    alloc.type = t;
    auto result = emit(out, alloc);
    for (uint32_t n = 0; n < t.width; ++n)
      store(out, result, load(out, buffer, offset + n, {T::Scalar, 1, 64}), n);
    return result;
  }
  void destroy(std::vector<I> &out, TypeId tid, runtime::Value value,
               const std::vector<uint32_t> &moved = {}) {
    const auto &t = core.semantics.types[tid];
    if (core.semantics.heap(tid)) {
      I drop{};
      drop.kind = I::Free;
      drop.args = {value};
      if (t.representation == hir::Representation::StringRecord ||
          t.representation == hir::Representation::InputRecord) {
        drop.kind = I::Call;
        drop.callee = t.representation == hir::Representation::StringRecord ? "isac_rt_string_drop"
                                                                            : "isac_rt_input_drop";
      }
      emit(out, drop, false);
    } else if (t.kind == hir::Type::Enum) {
      auto tag = load(out, value, 0, {T::Scalar, 1, 32});
      for (const auto &variant : t.variants) {
        std::vector<I> body;
        for (size_t n = 0; n < variant.payload.size(); ++n) {
          auto offset = core.semantics.payloadOffset(tid, variant.id, n);
          auto pt = variant.payload[n];
          if (!core.semantics.needsDrop(pt) ||
              std::find(moved.begin(), moved.end(), offset) != moved.end())
            continue;
          destroy(body, pt, project(body, value, offset, pt));
        }
        if (body.empty())
          continue;
        I compare{};
        compare.kind = I::Binary;
        compare.callee = "==";
        compare.type = {T::Scalar, 1, 32};
        compare.args = {tag, constant(out, variant.id)};
        auto condition = emit(out, compare);
        I branch{};
        branch.kind = I::If;
        branch.args = {condition};
        branch.body = std::move(body);
        emit(out, branch, false);
      }
    }
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
        auto found = c.movedPayloads.find(sid);
        destroy(out, core.semantics.symbols[sid].type, slots.at(sid),
                found == c.movedPayloads.end() ? std::vector<uint32_t>{} : found->second);
      }
      if (c.freeTemporary)
        destroy(out, c.type, i.args.at(0));
      switch (c.kind) {
      case C::StoreReference:
        values[c.result] = slots.at(c.slot);
        break;
      case C::ReferenceArgument:
        values[c.result] = referenceView(out, slots.at(c.slot), c.type);
        break;
      case C::StaticStr:
        i.kind = I::StaticStr;
        i.callee = c.text;
        values[c.result] = emit(out, i);
        break;
      case C::Length:
        i.kind = I::Length;
        i.args = {slots.at(c.slot)};
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
        auto zero = constant(out, 0);
        for (uint32_t n = 0; n < i.type.width; ++n)
          store(out, v, zero, n);
        store(out, v, constant(out, c.offset), 0);
        const auto &payload = core.semantics.types[c.type].variants.at(c.offset).payload;
        for (size_t n = 0; n < c.args.size(); ++n) {
          auto offset = core.semantics.payloadOffset(c.type, c.offset, n);
          auto value = values.at(c.args[n]);
          auto pt = type(payload[n]);
          if (pt.kind == T::Buffer) {
            for (uint32_t k = 0; k < pt.width; ++k)
              store(out, v, load(out, value, k, {T::Scalar, 1, 64}), offset + k);
          } else
            store(out, v, value, offset);
        }
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
        i.args.insert(i.args.begin(), slots.at(c.slot));
        if (c.kind == C::Read && !c.args.empty()) {
          I check{};
          check.kind = I::IndexCheck;
          check.args = i.args;
          emit(out, check, false);
        }
        i.offset = c.offset;
        values[c.result] = emit(out, i);
        break;
      case C::PayloadTake:
        values[c.result] = project(out, slots.at(c.slot), c.offset, c.type);
        break;
      case C::Move:
        values[c.result] = slots.at(c.slot);
        break;
      case C::Write:
        if (c.args.size() == 2) {
          I check{};
          check.kind = I::IndexCheck;
          check.args = {slots.at(c.slot), i.args[1]};
          emit(out, check, false);
          i.kind = I::Store;
          i.args.insert(i.args.begin(), slots.at(c.slot));
          emit(out, i, false);
        } else
          store(out, slots.at(c.slot), i.args[0], c.offset);
        break;
      case C::BorrowShared:
      case C::BorrowUnique:
        if (c.placeProjection != C::PlaceProjection::None) {
          auto source = slots.at(c.slot);
          runtime::Value index;
          if (c.placeProjection == C::PlaceProjection::Field) {
            auto sourceType = core.semantics.symbols[c.slot].type;
            index =
                constant(out, core.semantics.types[sourceType].fields[c.fieldIndex].offset, true);
          } else {
            index = i.args.at(0);
            I check{};
            check.kind = I::IndexCheck;
            check.args = {source, index};
            emit(out, check, false);
          }
          i.kind = I::Subview;
          i.type = referenceType(c.type);
          i.args = {source, index};
          slots[c.offset] = emit(out, i);
        } else if (!c.args.empty())
          slots[c.offset] = values.at(c.args[0]);
        else if (c.projection) {
          auto value = project(out, slots.at(c.slot), c.projection, c.type);
          if (scalar(c.type)) {
            I alloc{};
            alloc.kind = I::Alloc;
            alloc.type = {T::Buffer, 1, 64};
            auto buffer = emit(out, alloc);
            store(out, buffer, value);
            value = buffer;
          }
          slots[c.offset] = value;
        } else if (core.semantics.types[c.type].kind == hir::Type::Slice &&
                   core.semantics.types[core.semantics.symbols[c.slot].type].kind ==
                       hir::Type::Array) {
          i.kind = I::View;
          i.args = {slots.at(c.slot)};
          slots[c.offset] = emit(out, i);
        } else
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
        i.callee = core.semantics.functions[c.function].intrinsic.empty()
                       ? name(c.function)
                       : intrinsicRuntimeSymbol(core.semantics.functions[c.function].intrinsicId);
        if (c.slot != None) {
          auto source = slots.at(c.slot);
          auto tid = core.semantics.symbols[c.slot].type;
          if (scalar(tid))
            source = referenceView(out, source, tid);
          i.args.insert(i.args.begin(), source);
        }
        if (core.semantics.functions[c.function].returnsReference()) {
          i.type = referenceType(c.type);
          values[c.result] = emit(out, i);
        } else if (i.type.kind == T::Buffer || recordWords(c.type)) {
          I alloc{};
          alloc.kind = recordWords(c.type) ? I::RecordAlloc : I::Alloc;
          alloc.literal = recordWords(c.type);
          alloc.type = i.type;
          auto destination = emit(out, alloc);
          values[c.result] = destination;
          i.args.insert(i.args.begin(), destination);
          i.type = {T::Void, 0};
          emit(out, i, false);
        } else if (c.type == Unit)
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
        if (returnStorage != runtime::NoValue) {
          auto source = i.args.at(0);
          if (auto words = recordWords(returnType)) {
            I copy{};
            copy.kind = I::MemCopy;
            copy.type = {T::Pointer, 1};
            copy.args = {returnStorage, source,
                         constant(out, words * (core.semantics.target.pointerWidth / 8), true)};
            emit(out, copy);
          } else {
            for (uint32_t n = 0; n < type(returnType).width; ++n)
              store(out, returnStorage, load(out, source, n, {T::Scalar, 1, 64}), n);
          }
          i.args.clear();
        }
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
      returnStorage = runtime::NoValue;
      returnType = hf.result;
      if (hf.returnsReference()) {
        f.result = referenceType(hf.result);
      } else if (f.result.kind == T::Buffer || recordWords(hf.result)) {
        returnStorage = p.values++;
        runtimeTypes[returnStorage] = f.result;
        f.params.push_back({returnStorage, f.result});
        f.result = {T::Void, 0};
      }
      std::vector<I> prefix;
      for (auto sid : hf.params) {
        auto t = type(core.semantics.symbols[sid].type);
        if ((core.semantics.symbols[sid].receiver || core.semantics.symbols[sid].reference) &&
            scalar(core.semantics.symbols[sid].type))
          t = referenceType(core.semantics.symbols[sid].type);
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

    runtime::Function entry;
    entry.name = "main";
    entry.result = type(I32);
    entry.params = {{p.values++, type(I32)}, {p.values++, type(RawPtr)}};
    bool environment = std::any_of(core.semantics.functions.begin(), core.semantics.functions.end(),
                                   [](const auto &f) {
                                     return f.intrinsicId == IntrinsicId::Env_argument ||
                                            f.intrinsicId == IntrinsicId::Env_argument_count;
                                   });
    if (environment) {
      I initialize{};
      initialize.kind = I::Call;
      initialize.callee = "isac_rt_env_init";
      initialize.args = {entry.params[0].value, entry.params[1].value};
      emit(entry.body, initialize, false);
    }
    I call{};
    call.kind = I::Call;
    call.callee = name(core.semantics.entryPoint.function);
    bool unit = core.semantics.entryPoint.signature == hir::MainSignatureKind::NoArgsUnit;
    call.type = type(unit ? Unit : I32);
    auto code = emit(entry.body, call, !unit);
    if (unit)
      code = constant(entry.body, 0);
    if (environment) {
      I cleanup{};
      cleanup.kind = I::Call;
      cleanup.callee = "isac_rt_env_destroy";
      emit(entry.body, cleanup, false);
    }
    I ret{};
    ret.kind = I::Return;
    ret.args = {code};
    emit(entry.body, ret, false);
    p.functions.push_back(std::move(entry));
    return std::move(p);
  }
};
}
runtime::Erased erase(const core::Verified &v) {
  return Eraser(v).run();
}
}
