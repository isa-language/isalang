#include "isac/ffi.hpp"
#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <sstream>
namespace isac::ffi {
bool validName(const std::string &name) {
  return !name.empty() && (std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_') &&
         std::all_of(name.begin(), name.end(),
                     [](unsigned char c) { return std::isalnum(c) || c == '_'; });
}
std::pair<uint32_t, uint32_t> layout(const hir::Program &p, TypeId id) {
  if (p.rawPointer(id) || p.functionPointer(id))
    return {p.target.pointerWidth / 8, p.target.pointerAlignment};
  if (id == Bool)
    return {1, 1};
  if (integer(id)) {
    auto bytes = bitWidth(id, p.target.pointerWidth) / 8;
    auto index = bytes == 1 ? 0 : bytes == 2 ? 1 : bytes == 4 ? 2 : 3;
    return {bytes, p.target.integerAlignment[index]};
  }
  if (floating(id))
    return {id == F32 ? 4u : 8u, p.target.floatAlignment[id == F32 ? 0 : 1]};
  const auto &t = p.types.at(id);
  if (t.kind == hir::Type::Array && p.cScalar(t.element)) {
    auto [size, alignment] = layout(p, t.element);
    return {size * t.count, alignment};
  }
  if (t.kind != hir::Type::ForeignRecord)
    throw std::runtime_error("type has no sized C representation");
  uint32_t size = 0, alignment = 1;
  for (const auto &field : t.fields) {
    auto [bytes, align] = layout(p, field.type);
    size = t.foreignUnion ? std::max(size, bytes) : (size + align - 1) / align * align + bytes;
    alignment = std::max(alignment, align);
  }
  return {(size + alignment - 1) / alignment * alignment, alignment};
}
ParameterKind parameterKind(const hir::Program &p, const hir::Symbol &s) {
  if (s.type == Str ||
      (s.reference && p.types[s.type].representation == hir::Representation::StringRecord))
    return ParameterKind::Text;
  if (!s.reference)
    return ParameterKind::Scalar;
  if (p.types[s.type].kind == hir::Type::Array)
    return ParameterKind::Array;
  if (p.types[s.type].kind == hir::Type::Slice)
    return ParameterKind::Slice;
  return ParameterKind::BorrowedScalar;
}
unsigned parameterCells(const hir::Program &p, const hir::Function &f) {
  unsigned count = 0;
  for (auto id : f.params)
    count += parameterKind(p, p.symbols[id]) == ParameterKind::Slice ? 2 : 1;
  return std::max(1u, count);
}
bool signature(const hir::Program &p, const hir::Function &f) {
  if (!validName(f.cLinkName.empty() ? f.name : f.cLinkName))
    return false;
  if (!f.foreignC || f.trusted || f.owner != None || !f.intrinsic.empty() ||
      f.intrinsicId != IntrinsicId::None || f.resultOrigin != None || f.resultRegion != None ||
      !f.additionalResultOrigins.empty() || f.resultPayloadSource ||
      !f.additionalPayloadSources.empty() || f.resultBorrowedContainer ||
      !f.payloadParameterRegions.empty() || (f.result != Unit && !p.cScalar(f.result)) ||
      f.name == "main" || f.name.starts_with("isac_") || f.name.starts_with("__isac_"))
    return false;
  if (!f.cLinkName.empty() && (f.cLinkName == "main" || f.cLinkName.starts_with("isac_") ||
                               f.cLinkName.starts_with("__isac_")))
    return false;
  if (f.variadic) {
    if (!f.external || f.fixedParams == 0 || f.fixedParams > f.params.size())
      return false;
    if (f.variadicDeclaration == None) {
      if (f.fixedParams != f.params.size())
        return false;
    } else {
      if (f.variadicDeclaration >= f.id || f.variadicDeclaration >= p.functions.size())
        return false;
      const auto &original = p.functions[f.variadicDeclaration];
      if (!original.variadic || original.variadicDeclaration != None || !signature(p, original) ||
          original.fixedParams != f.fixedParams || original.result != f.result ||
          (original.cLinkName.empty() ? original.name : original.cLinkName) != f.cLinkName ||
          !std::equal(original.params.begin(), original.params.end(), f.params.begin()))
        return false;
      for (size_t n = f.fixedParams; n < f.params.size(); ++n) {
        const auto &s = p.symbols[f.params[n]];
        if (s.reference || s.type == Bool || s.type == I8 || s.type == U8 || s.type == I16 ||
            s.type == U16 || s.type == F32 || !p.cScalar(s.type))
          return false;
      }
    }
  } else if (f.variadicDeclaration != None)
    return false;
  unsigned regions = 0;
  for (auto id : f.params) {
    const auto &s = p.symbols.at(id);
    if (s.receiver || (s.reference && s.access != Access::Shared && s.access != Access::Unique) ||
        (!s.reference && s.access != Access::Own))
      return false;
    auto kind = parameterKind(p, s);
    if (s.reference) {
      ++regions;
      if (s.region == None ||
          std::count_if(f.parameterRegions.begin(), f.parameterRegions.end(), [&](auto &r) {
            return r.parameter == id && r.region == s.region &&
                   r.kind == (s.access == Access::Unique ? BorrowKind::Unique : BorrowKind::Shared);
          }) != 1)
        return false;
    }
    if (kind == ParameterKind::Scalar && !p.cScalar(s.type))
      return false;
    if (kind == ParameterKind::BorrowedScalar && !p.cScalar(s.type))
      return false;
    if ((kind == ParameterKind::Array || kind == ParameterKind::Slice) &&
        !p.cScalar(p.types[s.type].element))
      return false;
    if (kind == ParameterKind::Text &&
        (s.access == Access::Unique || (s.type != Str && !s.reference)))
      return false;
  }
  return regions == f.parameterRegions.size();
}
std::string importName(FunctionId id) {
  return "__isac_c_import_" + std::to_string(id);
}
std::string exportName(FunctionId id) {
  return "__isac_c_export_" + std::to_string(id);
}
bool callbackSignature(const hir::Program &p, TypeId id, const hir::Function &f) {
  if (!p.functionPointer(id) || !signature(p, f) || f.variadic || f.parameterRegions.size() ||
      f.result != p.types[id].element || f.params.size() != p.types[id].parameters.size())
    return false;
  for (size_t n = 0; n < f.params.size(); ++n)
    if (p.symbols[f.params[n]].reference ||
        p.symbols[f.params[n]].type != p.types[id].parameters[n])
      return false;
  return true;
}
std::string addressName(FunctionId id) {
  return "__isac_c_address_" + std::to_string(id);
}
std::string indirectName(TypeId id) {
  return "__isac_c_indirect_" + std::to_string(id);
}
namespace {
std::string ctype(const hir::Program &p, TypeId id) {
  if (id == Unit)
    return "void";
  if (p.rawPointer(id))
    return "isac_ptr_" + std::to_string(id);
  if (p.functionPointer(id))
    return "isac_fn_" + std::to_string(id);
  if (id == F32)
    return "float";
  if (id == F64)
    return "double";
  if (id == Bool)
    return "_Bool";
  if (id == ISize)
    return "__PTRDIFF_TYPE__";
  if (id == USize)
    return "__SIZE_TYPE__";
  if (integer(id))
    return std::string("__") + (signedType(id) ? "INT" : "UINT") + std::to_string(bitWidth(id)) +
           "_TYPE__";
  return std::string(p.types[id].foreignUnion ? "union " : "struct ") +
         (p.types[id].cName.empty() ? p.types[id].name : p.types[id].cName);
}
std::string decode(const hir::Program &p, TypeId t, const std::string &cell) {
  if (t == F32)
    return "isac_f32(" + cell + ")";
  if (t == F64)
    return "isac_f64(" + cell + ")";
  if (p.rawPointer(t) || p.functionPointer(t))
    return "(" + ctype(p, t) + ")(__UINTPTR_TYPE__)" + cell;
  return "(" + ctype(p, t) + ")" + cell;
}
std::string encode(const hir::Program &p, TypeId t, const std::string &value) {
  if (t == F32)
    return "isac_bits_f32(" + value + ")";
  if (t == F64)
    return "isac_bits_f64(" + value + ")";
  if (p.rawPointer(t) || p.functionPointer(t))
    return "(isac_cell)(__UINTPTR_TYPE__)" + value;
  return "(isac_cell)" + value;
}
}
std::string bridge(const hir::Program &p) {
  if (std::none_of(p.functions.begin(), p.functions.end(), [](auto &f) { return f.foreignC; }) &&
      std::none_of(p.types.begin(), p.types.end(),
                   [](auto &t) { return t.kind == hir::Type::FunctionPointer; }))
    return {};
  std::ostringstream o;
  o << "typedef __UINT64_TYPE__ isac_cell;\n"
       "static float isac_f32(isac_cell x){__UINT32_TYPE__ b=x;float "
       "v;__builtin_memcpy(&v,&b,4);return v;}\n"
       "static double isac_f64(isac_cell x){double v;__builtin_memcpy(&v,&x,8);return v;}\n"
       "static isac_cell isac_bits_f32(float v){__UINT32_TYPE__ b;__builtin_memcpy(&b,&v,4);return "
       "b;}\n"
       "static isac_cell isac_bits_f64(double v){isac_cell b;__builtin_memcpy(&b,&v,8);return "
       "b;}\n";
  o << "struct isac_text{const __UINT8_TYPE__ *ptr;__SIZE_TYPE__ len;};\n"
       "struct isac_string{__UINT8_TYPE__ *ptr;__SIZE_TYPE__ len,capacity;};\n"
       "extern void *isac_rt_alloc(__SIZE_TYPE__);extern void isac_rt_free(void *);\n"
       "extern void isac_rt_panic(const char *);extern void isac_rt_ffi_utf8(const __UINT8_TYPE__ "
       "*,__SIZE_TYPE__);\n"
       "extern void isac_rt_string_from(struct isac_string *,const struct isac_text *);\n"
       "extern void isac_rt_string_drop(struct isac_string *);\n";
  for (const auto &function : p.functions)
    if (function.variadic) {
      o << "_Static_assert(sizeof(int)==4,\"C integer promotion target unsupported\");\n";
      break;
    }
  for (const auto &t : p.types)
    if (t.kind == hir::Type::ForeignRecord || t.kind == hir::Type::ForeignOpaque)
      o << (t.foreignUnion ? "union " : "struct ") << (t.cName.empty() ? t.name : t.cName) << ";\n";
  std::set<TypeId> pointers;
  std::function<void(TypeId)> pointer = [&](TypeId id) {
    if ((!p.rawPointer(id) && !p.functionPointer(id)) || !pointers.insert(id).second)
      return;
    const auto &t = p.types[id];
    pointer(t.element);
    if (p.functionPointer(id)) {
      for (auto param : t.parameters)
        pointer(param);
      o << "typedef " << ctype(p, t.element) << " (*" << ctype(p, id) << ")(";
      if (t.parameters.empty())
        o << "void";
      for (size_t n = 0; n < t.parameters.size(); ++n)
        o << (n ? "," : "") << ctype(p, t.parameters[n]);
      o << ");\n_Static_assert(sizeof(" << ctype(p, id)
        << ")==sizeof(__UINTPTR_TYPE__),\"function pointer representation unsupported\");\n";
    } else
      o << "typedef " << ctype(p, t.element) << (t.mutablePointer ? " * " : " const * ")
        << ctype(p, id) << ";\n";
  };
  for (TypeId id = 0; id < p.types.size(); ++id)
    pointer(id);

  std::set<TypeId> emitted;
  std::map<std::string, TypeId> tags;
  std::function<void(TypeId)> record = [&](TypeId id) {
    const auto &t = p.types[id];
    if (t.kind != hir::Type::ForeignRecord || !emitted.insert(id).second)
      return;
    for (const auto &field : t.fields)
      record(field.type);
    auto tag = t.cName.empty() ? t.name : t.cName;
    if (auto prior = tags.find(tag); prior != tags.end()) {
      const auto &other = p.types[prior->second];
      bool same = t.foreignUnion == other.foreignUnion &&
                  layout(p, id) == layout(p, prior->second) &&
                  t.fields.size() == other.fields.size();
      for (size_t n = 0; same && n < t.fields.size(); ++n)
        same = t.fields[n].name == other.fields[n].name &&
               t.fields[n].offset == other.fields[n].offset &&
               (p.types[t.fields[n].type].kind == hir::Type::Array
                    ? p.types[other.fields[n].type].kind == hir::Type::Array &&
                          p.types[t.fields[n].type].count == p.types[other.fields[n].type].count &&
                          ctype(p, p.types[t.fields[n].type].element) ==
                              ctype(p, p.types[other.fields[n].type].element)
                    : ctype(p, t.fields[n].type) == ctype(p, other.fields[n].type));
      if (!same)
        fail({}, DiagnosticCode::InvalidCAbi, "conflicting C declarations of struct `" + tag + "`");
      return;
    }
    tags[tag] = id;
    o << ctype(p, id) << "{\n";
    for (const auto &field : t.fields) {
      const auto &member = p.types[field.type];
      if (member.kind == hir::Type::Array)
        o << ctype(p, member.element) << " " << field.name << "[" << member.count << "];\n";
      else
        o << ctype(p, field.type) << " " << field.name << ";\n";
    }
    o << "};\n";
    auto [size, align] = layout(p, id);
    o << "_Static_assert(sizeof(" << ctype(p, id) << ")==" << size << ",\"C size mismatch\");\n";
    o << "_Static_assert(_Alignof(" << ctype(p, id) << ")==" << align
      << ",\"C alignment mismatch\");\n";
    for (const auto &field : t.fields)
      o << "_Static_assert(__builtin_offsetof(" << ctype(p, id) << "," << field.name
        << ")==" << field.offset << ",\"C field offset mismatch\");\n";
  };
  for (TypeId id = 0; id < p.types.size(); ++id)
    record(id);
  for (const auto &f : p.functions) {
    if (!f.foreignC)
      continue;
    auto kind = [&](size_t n) { return parameterKind(p, p.symbols[f.params[n]]); };
    auto element = [&](size_t n) {
      auto t = p.symbols[f.params[n]].type;
      return kind(n) == ParameterKind::Array || kind(n) == ParameterKind::Slice ? p.types[t].element
                                                                                : t;
    };
    auto prototype = [&] {
      o << ctype(p, f.result) << " " << (f.cLinkName.empty() ? f.name : f.cLinkName) << "(";
      if (f.params.empty())
        o << "void";
      auto fixed = f.variadic ? f.fixedParams : f.params.size();
      for (size_t n = 0; n < fixed; ++n) {
        if (n)
          o << ",";
        auto k = kind(n);
        const auto &param = p.symbols[f.params[n]];
        if (k == ParameterKind::Text)
          o << "const __UINT8_TYPE__ *a" << n << ",__SIZE_TYPE__ n" << n;
        else {
          o << ctype(p, element(n));
          if (k != ParameterKind::Scalar)
            o << (param.access == Access::Unique ? " *" : " const *");
          o << " a" << n;
          if (k == ParameterKind::Slice)
            o << ",__SIZE_TYPE__ n" << n;
        }
      }
      if (f.variadic)
        o << ",...";
      o << ")";
    };
    if (f.external) {
      o << "extern ";
      prototype();
      o << ";\nvoid " << importName(f.id) << "(isac_cell *out,const isac_cell *in){\n";
    } else {
      o << "extern void " << exportName(f.id) << "(isac_cell *,const isac_cell *);\n";
      prototype();
      o << "{isac_cell in[" << parameterCells(p, f) << "]={0},out[1]={0};\n";
    }
    unsigned cell = 0;
    for (size_t n = 0; n < f.params.size(); ++n) {
      auto k = kind(n);
      auto name = std::to_string(n);
      auto pos = std::to_string(cell);
      const auto &param = p.symbols[f.params[n]];
      if (k == ParameterKind::Scalar) {
        if (!f.external)
          o << "in[" << cell << "]=" << encode(p, param.type, "a" + name) << ";\n";
      } else if (k == ParameterKind::Text) {
        if (f.external)
          o << "const struct isac_text *t" << n
            << "=(const struct isac_text *)(__UINTPTR_TYPE__)in[" << cell << "];\n";
        else {
          o << "isac_rt_ffi_utf8(a" << n << ",n" << n << ");\n";
          if (param.type == Str)
            o << "struct isac_text t" << n << "={a" << n << ",n" << n << "};\n";
          else
            o << "struct isac_string t" << n << "={0};isac_rt_string_from(&t" << n
              << ",&(struct isac_text){a" << n << ",n" << n << "});\n";
          o << "in[" << cell << "]=(isac_cell)(__UINTPTR_TYPE__) &t" << n << ";\n";
        }
      } else {
        std::string count =
            k == ParameterKind::Slice
                ? (f.external ? "in[" + std::to_string(cell + 1) + "]" : "n" + name)
                : std::to_string(k == ParameterKind::Array ? p.types[param.type].count : 1);
        auto ct = ctype(p, element(n));
        o << "__SIZE_TYPE__ count" << n << "=" << count << ";\n"
          << "if(count" << n << ">(__SIZE_TYPE__)-1/sizeof(" << (f.external ? ct : "isac_cell")
          << "))isac_rt_panic(\"C sequence size overflow\");\n";
        if (f.external)
          o << "isac_cell *cells" << n << "=(isac_cell *)(__UINTPTR_TYPE__)in[" << cell << "];\n"
            << ct << " *a" << n << "=isac_rt_alloc(count" << n << "?count" << n << "*sizeof(" << ct
            << "):1);\n"
            << "for(__SIZE_TYPE__ j=0;j<count" << n << ";++j)a" << n
            << "[j]=" << decode(p, element(n), "cells" + name + "[j]") << ";\n";
        else
          o << "isac_cell *cells" << n << "=isac_rt_alloc(count" << n << "?count" << n
            << "*sizeof(isac_cell):1);\n"
            << "for(__SIZE_TYPE__ j=0;j<count" << n << ";++j)cells" << n
            << "[j]=" << encode(p, element(n), "a" + name + "[j]") << ";\n"
            << "in[" << cell << "]=(isac_cell)(__UINTPTR_TYPE__)cells" << n << ";\n";
        if (!f.external && k == ParameterKind::Slice)
          o << "in[" << cell + 1 << "]=count" << n << ";\n";
      }
      cell += k == ParameterKind::Slice ? 2 : 1;
    }
    if (f.external) {
      if (f.result != Unit)
        o << ctype(p, f.result) << " result=";
      o << (f.cLinkName.empty() ? f.name : f.cLinkName) << "(";
      cell = 0;
      for (size_t n = 0; n < f.params.size(); ++n) {
        if (n)
          o << ",";
        auto k = kind(n);
        if (k == ParameterKind::Scalar)
          o << decode(p, p.symbols[f.params[n]].type, "in[" + std::to_string(cell) + "]");
        else if (k == ParameterKind::Text)
          o << "t" << n << "->ptr,t" << n << "->len";
        else {
          o << "a" << n;
          if (k == ParameterKind::Slice)
            o << ",count" << n;
        }
        cell += k == ParameterKind::Slice ? 2 : 1;
      }
      o << ");\n";
    } else
      o << exportName(f.id) << "(out,in);\n";
    for (size_t n = 0; n < f.params.size(); ++n) {
      auto k = kind(n);
      auto name = std::to_string(n);
      const auto &param = p.symbols[f.params[n]];
      if (k == ParameterKind::Text && !f.external && param.type != Str)
        o << "isac_rt_string_drop(&t" << n << ");\n";
      if (k == ParameterKind::Scalar || k == ParameterKind::Text)
        continue;
      if (param.access == Access::Unique)
        o << "for(__SIZE_TYPE__ j=0;j<count" << n << ";++j)"
          << (f.external ? "cells" + name + "[j]=" + encode(p, element(n), "a" + name + "[j]")
                         : "a" + name + "[j]=" + decode(p, element(n), "cells" + name + "[j]"))
          << ";\n";
      o << "isac_rt_free(" << (f.external ? "a" : "cells") << n << ");\n";
    }
    if (f.result != Unit) {
      if (f.external)
        o << "out[0]=" << encode(p, f.result, "result") << ";\n";
      else
        o << "return " << decode(p, f.result, "out[0]") << ";\n";
    }
    o << "}\n";
  }

  for (const auto &f : p.functions)
    for (TypeId id = 0; id < p.types.size(); ++id)
      if (callbackSignature(p, id, f)) {
        o << "void " << addressName(f.id) << "(isac_cell *out,const isac_cell *in){"
          << "(void)in;out[0]=" << encode(p, id, "&" + (f.cLinkName.empty() ? f.name : f.cLinkName))
          << ";}\n";
        break;
      }
  for (TypeId id = 0; id < p.types.size(); ++id) {
    if (!p.functionPointer(id))
      continue;
    const auto &t = p.types[id];
    o << "void " << indirectName(id) << "(isac_cell *out,const isac_cell *in){" << ctype(p, id)
      << " fn=" << decode(p, id, "in[0]") << ";"
      << "if(!fn)isac_rt_panic(\"null C function pointer\");";
    if (t.element != Unit)
      o << ctype(p, t.element) << " result=";
    o << "fn(";
    for (size_t n = 0; n < t.parameters.size(); ++n)
      o << (n ? "," : "") << decode(p, t.parameters[n], "in[" + std::to_string(n + 1) + "]");
    o << ");";
    if (t.element != Unit)
      o << "out[0]=" << encode(p, t.element, "result") << ";";
    else
      o << "(void)out;";
    o << "}\n";
  }
  return o.str();
}
}
