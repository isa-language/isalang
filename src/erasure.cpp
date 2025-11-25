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
  uint32_t cleanupLoop = UINT32_MAX;
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
    auto result = scalar(t) ? T{T::Buffer, 1, 64} : type(t);
    if (result.kind == T::Buffer)
      result.dynamicOffset = true;
    return result;
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
  runtime::Value abi(std::vector<I> &out, const std::string &callee, T result,
                     std::vector<runtime::Value> args) {
    I call{};
    call.kind = I::Call;
    call.callee = callee;
    call.type = result;
    call.args = std::move(args);
    return emit(out, call, result.kind != T::Void);
  }
  runtime::Value storageView(std::vector<I> &out, runtime::Value pointer, TypeId element) {
    I view{};
    view.kind = I::AddressView;
    view.type = {T::Buffer, core.semantics.storageWidth(element), 64};
    view.args = {pointer};
    return emit(out, view);
  }
  void writeElement(std::vector<I> &out, runtime::Value storage, runtime::Value value,
                    TypeId element, uint32_t offset = 0) {
    if (type(element).kind == T::Buffer)
      for (uint32_t n = 0; n < core.semantics.storageWidth(element); ++n)
        store(out, storage, load(out, value, n, {T::Scalar, 1, 64}), offset + n);
    else
      store(out, storage, value, offset);
  }

  runtime::Value transferElement(std::vector<I> &out, TypeId tid, runtime::Value value,
                                 bool toHeap) {
    if (auto words = recordWords(tid)) {
      auto bytes = constant(out, words * (core.semantics.target.pointerWidth / 8), true);
      I alloc{};
      alloc.type = {T::Pointer, 1};
      alloc.kind = toHeap ? I::HeapAlloc : I::RecordAlloc;
      alloc.literal = words;
      if (toHeap)
        alloc.args = {bytes};
      auto target = emit(out, alloc);
      I copy{};
      copy.kind = I::MemCopy;
      copy.type = {T::Pointer, 1};
      copy.args = {target, value, bytes};
      emit(out, copy);
      if (!toHeap) {
        I free{};
        free.kind = I::Free;
        free.args = {value};
        emit(out, free, false);
      }
      return target;
    }
    const auto &t = core.semantics.types[tid];
    if (core.semantics.heap(tid) || type(tid).kind != T::Buffer)
      return value;
    auto result = project(out, value, 0, tid);
    if (t.kind == hir::Type::Structure) {
      for (const auto &field : t.fields)
        if (core.semantics.needsDrop(field.type)) {
          auto member = project(out, value, field.offset, field.type);
          writeElement(out, result, transferElement(out, field.type, member, toHeap), field.type,
                       field.offset);
        }
    } else if (t.kind == hir::Type::Enum) {
      auto tag = load(out, value, 0, {T::Scalar, 1, 32});
      for (const auto &variant : t.variants) {
        std::vector<I> body;
        for (size_t n = 0; n < variant.payload.size(); ++n) {
          auto pt = variant.payload[n];
          if (!core.semantics.needsDrop(pt))
            continue;
          auto offset = core.semantics.payloadOffset(tid, variant.id, n);
          auto member = project(body, value, offset, pt);
          writeElement(body, result, transferElement(body, pt, member, toHeap), pt, offset);
        }
        if (body.empty())
          continue;
        I compare{};
        compare.kind = I::Binary;
        compare.callee = "==";
        compare.type = {T::Scalar, 1, 32};
        compare.args = {tag, constant(out, variant.id)};
        I branch{};
        branch.kind = I::If;
        branch.args = {emit(out, compare)};
        branch.body = std::move(body);
        emit(out, branch, false);
      }
    }
    return result;
  }
  void destroyVector(std::vector<I> &out, TypeId tid, runtime::Value value, bool clear) {
    auto element = core.semantics.types[tid].element;
    if (core.semantics.needsDrop(element)) {
      T size{T::Size, 1, core.semantics.target.pointerWidth};
      auto len = abi(out, "isac_rt_vec_len", size, {value});
      I alloc{};
      alloc.kind = I::Alloc;
      alloc.type = {T::Buffer, 1, 64};
      auto counter = emit(out, alloc);
      store(out, counter, len);
      I loop{};
      loop.kind = I::Loop;
      loop.loop = cleanupLoop--;
      loop.unconditional = true;
      auto index = load(loop.body, counter, 0, size);
      I compare{};
      compare.kind = I::Binary;
      compare.callee = "==";
      compare.type = {T::Scalar, 1, 32};
      compare.args = {index, constant(loop.body, 0, true)};
      I done{};
      done.kind = I::If;
      done.args = {emit(loop.body, compare)};
      I exit{};
      exit.kind = I::Break;
      exit.loop = loop.loop;
      done.body = {exit};
      emit(loop.body, done, false);
      I decrement{};
      decrement.kind = I::Binary;
      decrement.callee = "-";
      decrement.type = size;
      decrement.args = {index, constant(loop.body, 1, true)};
      auto next = emit(loop.body, decrement);
      store(loop.body, counter, next);
      auto ptr = abi(loop.body, "isac_rt_vec_at", {T::Pointer, 1}, {value, next});
      auto view = storageView(loop.body, ptr, element);
      destroy(loop.body, element, project(loop.body, view, 0, element), {}, {}, {}, true);
      emit(out, loop, false);
    }
    abi(out, clear ? "isac_rt_vec_clear_storage" : "isac_rt_vec_drop_storage", {T::Void, 0},
        {value});
  }
  runtime::Value containerCall(std::vector<I> &out, const core::Inst &c,
                               std::vector<runtime::Value> args) {
    const auto &function = core.semantics.functions[c.function];
    auto element = core.semantics.types[function.owner].element;
    T pointer{T::Pointer, 1}, size{T::Size, 1, core.semantics.target.pointerWidth};
    auto owner = c.slot == None ? runtime::NoValue : slots.at(c.slot);
    switch (c.intrinsicId) {
    case IntrinsicId::Box_new_generic: {
      I alloc{};
      alloc.kind = I::HeapAlloc;
      alloc.type = pointer;
      alloc.args = {constant(out, 8 * core.semantics.storageWidth(element), true)};
      auto result = emit(out, alloc);
      auto stored = transferElement(out, element, args[0], true);
      writeElement(out, storageView(out, result, element), stored, element);
      return result;
    }
    case IntrinsicId::Box_get: {
      auto view = storageView(out, owner, element);
      return core.semantics.heap(element) ? project(out, view, 0, element)
                                          : referenceView(out, view, element);
    }
    case IntrinsicId::Box_into_inner: {
      auto value = project(out, storageView(out, owner, element), 0, element);
      auto result = transferElement(out, element, value, false);
      I free{};
      free.kind = I::Free;
      free.args = {owner};
      emit(out, free, false);
      return result;
    }
    case IntrinsicId::Vec_new:
    case IntrinsicId::Vec_with_capacity:
      return abi(out, "isac_rt_vec_new", pointer,
                 {constant(out, 8 * core.semantics.storageWidth(element), true),
                  args.empty() ? constant(out, 0, true) : args[0]});
    case IntrinsicId::Vec_len:
      return abi(out, "isac_rt_vec_len", size, {owner});
    case IntrinsicId::Vec_capacity:
      return abi(out, "isac_rt_vec_capacity", size, {owner});
    case IntrinsicId::Vec_push: {
      auto ptr = abi(out, "isac_rt_vec_push_slot", pointer, {owner});
      auto stored = transferElement(out, element, args[0], true);
      writeElement(out, storageView(out, ptr, element), stored, element);
      return runtime::NoValue;
    }
    case IntrinsicId::Vec_clear:
      destroyVector(out, function.owner, owner, true);
      return runtime::NoValue;
    case IntrinsicId::Vec_get:
    case IntrinsicId::Vec_first:
    case IntrinsicId::Vec_last: {
      I alloc{};
      alloc.kind = I::Alloc;
      alloc.type = type(c.type);
      auto result = emit(out, alloc);
      auto len = abi(out, "isac_rt_vec_len", size, {owner});
      auto index = args.empty() ? constant(out, 0, true) : args[0];
      I compare{};
      compare.kind = I::Binary;
      compare.type = {T::Scalar, 1, 32};
      compare.callee = c.intrinsicId == IntrinsicId::Vec_last ? "==" : ">=";
      compare.unsignedOperands = true;
      compare.args = c.intrinsicId == IntrinsicId::Vec_last
                         ? std::vector<runtime::Value>{len, constant(out, 0, true)}
                         : std::vector<runtime::Value>{index, len};
      I branch{};
      branch.kind = I::If;
      branch.args = {emit(out, compare)};
      const auto &variants = core.semantics.types[c.type].variants;
      store(branch.body, result, constant(branch.body, variants[1].id));
      if (c.intrinsicId == IntrinsicId::Vec_last) {
        I subtract{};
        subtract.kind = I::Binary;
        subtract.type = size;
        subtract.callee = "-";
        subtract.args = {len, constant(branch.otherwise, 1, true)};
        index = emit(branch.otherwise, subtract);
      }
      auto address = abi(branch.otherwise, "isac_rt_vec_at", pointer, {owner, index});

      if (core.semantics.heap(element))
        address =
            project(branch.otherwise, storageView(branch.otherwise, address, element), 0, element);
      store(branch.otherwise, result, constant(branch.otherwise, variants[0].id));
      store(branch.otherwise, result, address,
            core.semantics.payloadOffset(c.type, variants[0].id, 0));
      emit(out, branch, false);
      return result;
    }
    case IntrinsicId::Vec_pop: {
      I alloc{};
      alloc.kind = I::Alloc;
      alloc.type = type(c.type);
      auto result = emit(out, alloc);
      auto len = abi(out, "isac_rt_vec_len", size, {owner});
      I compare{};
      compare.kind = I::Binary;
      compare.callee = "==";
      compare.type = {T::Scalar, 1, 32};
      compare.args = {len, constant(out, 0, true)};
      I branch{};
      branch.kind = I::If;
      branch.args = {emit(out, compare)};
      const auto &variants = core.semantics.types[c.type].variants;
      store(branch.body, result, constant(branch.body, variants[1].id));
      auto ptr = abi(branch.otherwise, "isac_rt_vec_pop_slot", pointer, {owner});
      auto view = storageView(branch.otherwise, ptr, element);
      auto value = project(branch.otherwise, view, 0, element);
      auto taken = transferElement(branch.otherwise, element, value, false);
      store(branch.otherwise, result, constant(branch.otherwise, variants[0].id));
      writeElement(branch.otherwise, result, taken, element,
                   core.semantics.payloadOffset(c.type, variants[0].id, 0));
      emit(out, branch, false);
      return result;
    }
    default:
      fail(c.loc, DiagnosticCode::InvalidCore, "unhandled verified container intrinsic");
    }
  }
  void destroy(std::vector<I> &out, TypeId tid, runtime::Value value,
               const std::vector<uint32_t> &moved = {},
               const std::vector<uint32_t> &absentFields = {}, const core::FieldDropPlan &plan = {},
               bool heapDescriptors = false) {
    if (plan.skip)
      return;
    const auto &t = core.semantics.types[tid];
    if (t.representation == hir::Representation::GenericBox) {
      auto view = storageView(out, value, t.element);
      destroy(out, t.element, project(out, view, 0, t.element), {}, {}, {}, true);
      I free{};
      free.kind = I::Free;
      free.args = {value};
      emit(out, free, false);
    } else if (t.representation == hir::Representation::GenericVec) {
      destroyVector(out, tid, value, false);
    } else if (core.semantics.heap(tid)) {
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
      if (heapDescriptors && recordWords(tid)) {
        I free{};
        free.kind = I::Free;
        free.args = {value};
        emit(out, free, false);
      }
    } else if (t.kind == hir::Type::Structure) {
      for (size_t n = t.fields.size(); n > 0; --n) {
        auto id = static_cast<uint32_t>(n - 1);
        const auto &field = t.fields[id];
        if (core.semantics.needsDrop(field.type) &&
            std::find(absentFields.begin(), absentFields.end(), id) == absentFields.end())
          destroy(out, field.type, project(out, value, field.offset, field.type), {}, {},
                  plan.fields.contains(id) ? plan.fields.at(id) : core::FieldDropPlan{},
                  heapDescriptors);
      }
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
          destroy(body, pt, project(body, value, offset, pt), {}, {}, {}, heapDescriptors);
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
        auto absent = c.absentFields.find(sid);
        destroy(out, core.semantics.symbols[sid].type, slots.at(sid),
                found == c.movedPayloads.end() ? std::vector<uint32_t>{} : found->second,
                absent == c.absentFields.end() ? std::vector<uint32_t>{} : absent->second,
                c.fieldCleanup.contains(sid) ? c.fieldCleanup.at(sid) : core::FieldDropPlan{});
      }
      if (c.freeTemporary)
        destroy(out, c.type, i.args.at(0), {}, c.temporaryAbsentFields, c.temporaryFieldPlan);
      switch (c.kind) {
      case C::StoreReference:
        if (!core.semantics.heap(core.semantics.types[c.type].element)) {
          i.kind = I::Address;
          i.type = {T::Pointer, 1};
          i.args = {slots.at(c.slot)};
          values[c.result] = emit(out, i);
        } else {
          values[c.result] = slots.at(c.slot);
        }
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
      case C::ContainerCall: {
        auto result = containerCall(out, c, i.args);
        if (c.result != None)
          values[c.result] = result;
        break;
      }
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
      case C::FieldStore:
      case C::FieldTake:
      case C::FieldDrop: {
        const auto &field =
            core.semantics.types[core.semantics.symbols[c.slot].type].fields.at(c.fieldIndex);
        if (c.replaceField)
          destroy(out, field.type, project(out, slots.at(c.slot), field.offset, field.type), {}, {},
                  c.replacedFieldPlan);
        if (c.kind == C::FieldStore) {
          if (type(field.type).kind == T::Buffer) {
            for (uint32_t n = 0; n < type(field.type).width; ++n)
              store(out, slots.at(c.slot), load(out, i.args.at(0), n, {T::Scalar, 1, 64}),
                    field.offset + n);
          } else
            store(out, slots.at(c.slot), i.args.at(0), field.offset);
        } else if (c.kind == C::FieldTake)
          values[c.result] = project(out, slots.at(c.slot), field.offset, field.type);
        break;
      }
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
            uint32_t offset = 0;
            for (auto field : c.fieldPath) {
              const auto &member = core.semantics.types[sourceType].fields.at(field);
              offset += member.offset;
              sourceType = member.type;
            }
            if (core.semantics.heap(c.type)) {
              slots[c.offset] = load(out, source, offset, type(c.type));
              break;
            }
            index = constant(out, offset, true);
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

          if (c.storedPayloadReference && !core.semantics.heap(c.type)) {
            auto address = load(out, slots.at(c.slot), c.projection, {T::Pointer, 1});
            i.kind = I::AddressView;
            i.type = referenceType(c.type);
            i.args = {address};
            slots[c.offset] = emit(out, i);
            break;
          }
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
          if (scalar(tid) || c.access != Access::Own)
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
        if (core.semantics.symbols[sid].reference ||
            (core.semantics.symbols[sid].receiver &&
             (scalar(core.semantics.symbols[sid].type) ||
              core.semantics.symbols[sid].access != Access::Own)))
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
