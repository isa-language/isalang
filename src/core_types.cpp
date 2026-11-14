#include "isac/core.hpp"
#include "isac/ffi.hpp"
#include <algorithm>
#include <functional>
#include <map>

namespace isac::core {

void checkTypes(const Program &p) {

  std::vector<unsigned> layoutState(p.semantics.types.size());
  std::function<void(TypeId)> layout = [&](TypeId id) {
    auto badLayout = [&] { fail({}, DiagnosticCode::InvalidCore, "invalid by-value type layout"); };
    if (id >= p.semantics.types.size())
      badLayout();
    if (layoutState[id] == 2)
      return;
    if (layoutState[id] == 1)
      badLayout();
    layoutState[id] = 1;
    const auto &type = p.semantics.types[id];
    if (type.representation == hir::Representation::FutureFrame) {
      if (type.futureFunction == None || type.element >= p.semantics.types.size())
        badLayout();
      layout(type.element);
    } else if (p.semantics.dynamicContainer(id)) {
      layout(type.element);
      if (type.kind != hir::Type::Structure || type.width != 1 || !type.fields.empty() ||
          type.protocol != None || type.dropGlue != hir::DropGlue::ContainerElements ||
          !p.semantics.containerElement(type.element))
        badLayout();
      bool instanceFound = false;
      for (const auto &instance : p.semantics.instances)
        if (instance.type == id && instance.declaration >= p.semantics.genericEnums.size()) {
          auto index = instance.declaration - p.semantics.genericEnums.size();
          if (index >= p.semantics.genericStructs.size())
            badLayout();
          const auto &decl = p.semantics.genericStructs[index];
          if (decl.representation != type.representation || decl.parameters.size() != 1 ||
              instance.substitution.types !=
                  std::vector<std::pair<TypeParamId, TypeId>>{{decl.parameters[0], type.element}})
            badLayout();
          instanceFound = true;
        }
      if (!instanceFound)
        badLayout();
    } else if (type.kind == hir::Type::Array || type.kind == hir::Type::Slice) {
      layout(type.element);
      if ((!scalar(type.element) && !(type.kind == hir::Type::Array &&
                                      p.semantics.types[type.element].kind == hir::Type::Array)) ||
          type.protocol != None || !type.fields.empty() ||
          type.dropGlue != hir::DropGlue::Trivial ||
          type.width != type.count * p.semantics.storageWidth(type.element) || type.width > 65536 ||
          (type.kind == hir::Type::Array && (!type.count || type.count > 65536)) ||
          (type.kind == hir::Type::Slice && type.count != 0))
        badLayout();
    } else if (type.kind == hir::Type::ForeignRecord) {
      if (type.fields.empty() || type.protocol != None || type.dropGlue != hir::DropGlue::Trivial ||
          type.representation != hir::Representation::Stack)
        badLayout();
      uint32_t bytes = 0, alignment = 1;
      for (const auto &field : type.fields) {
        const auto &member = p.semantics.types.at(field.type);
        if (!p.semantics.cScalar(field.type) && member.kind != hir::Type::ForeignRecord &&
            !(member.kind == hir::Type::Array && p.semantics.cScalar(member.element)))
          badLayout();
        layout(field.type);
        auto [size, align] = ffi::layout(p.semantics, field.type);
        if (type.foreignUnion) {
          if (field.offset != 0)
            badLayout();
          bytes = std::max(bytes, size);
        } else {
          bytes = (bytes + align - 1) / align * align;
          if (field.offset != bytes)
            badLayout();
          bytes += size;
        }
        alignment = std::max(alignment, align);
      }
      if (type.width != (bytes + alignment - 1) / alignment * alignment)
        badLayout();
    } else if (type.kind == hir::Type::FunctionPointer) {
      if (type.element >= p.semantics.types.size() || type.width != 1 || type.protocol != None ||
          type.dropGlue != hir::DropGlue::Trivial ||
          type.representation != hir::Representation::Stack || type.foreignUnion ||
          !type.fields.empty() || !type.variants.empty() ||
          (type.element != Unit && !p.semantics.cScalar(type.element)))
        badLayout();
      layout(type.element);
      for (auto parameter : type.parameters) {
        if (!p.semantics.cScalar(parameter))
          badLayout();
        layout(parameter);
      }
    } else if (type.kind == hir::Type::RawPointer) {
      if (type.element >= p.semantics.types.size() || type.width != 1 || type.protocol != None ||
          type.dropGlue != hir::DropGlue::Trivial ||
          type.representation != hir::Representation::Stack)
        badLayout();
      if (p.semantics.rawPointer(type.element))
        layout(type.element);
      auto k = p.semantics.types.at(type.element).kind;
      if (type.element != Unit && !p.semantics.cScalar(type.element) &&
          k != hir::Type::ForeignRecord && k != hir::Type::ForeignOpaque)
        badLayout();
    } else if (type.kind == hir::Type::Structure && !p.semantics.heap(id)) {
      uint32_t width = 0;
      for (const auto &field : type.fields) {
        layout(field.type);
        auto cells = p.semantics.storageWidth(field.type);
        if (cells > 65536 || width > 65536 - cells || field.offset != width)
          badLayout();
        width += cells;
      }
      if (type.width != std::max(1u, width))
        badLayout();
    } else if (type.kind == hir::Type::Enum) {
      uint32_t width = 1;
      for (const auto &variant : type.variants) {
        uint32_t cells = 1;
        for (auto payload : variant.payload) {
          layout(payload);
          auto size = p.semantics.storageWidth(payload);
          if (size > 65536 || cells > 65536 - size)
            badLayout();
          cells += size;
        }
        width = std::max(width, cells);
      }
      if (type.width != width)
        badLayout();
    }
    layoutState[id] = 2;
  };
  for (TypeId id = 0; id < p.semantics.types.size(); ++id)
    layout(id);
  for (const auto &type : p.semantics.types)
    if ((type.kind == hir::Type::ForeignRecord || type.kind == hir::Type::ForeignOpaque) &&
        !ffi::validName(type.cName.empty() ? type.name : type.cName))
      fail({}, DiagnosticCode::InvalidCore, "invalid C record tag");
  for (const auto &symbol : p.semantics.symbols)
    if (p.semantics.types.at(symbol.type).kind == hir::Type::ForeignRecord ||
        p.semantics.types.at(symbol.type).kind == hir::Type::ForeignOpaque)
      fail(symbol.loc, DiagnosticCode::InvalidCore, "C types cannot be carried by value");
  for (const auto &type : p.semantics.types) {
    if (type.kind == hir::Type::Structure)
      for (const auto &field : type.fields)
        if (p.semantics.containsReferences(field.type) &&
            (!p.semantics.localCarrierType(field.type) || type.protocol != None ||
             type.representation != hir::Representation::Stack))
          fail({}, DiagnosticCode::InvalidCore,
               "stored-reference field requires a recursive region contract");
    if (type.kind == hir::Type::Enum) {
      for (const auto &variant : type.variants)
        for (auto payload : variant.payload) {
          if (!p.semantics.reference(payload) && p.semantics.containsReferences(payload))
            fail({}, DiagnosticCode::InvalidCore,
                 "nested reference payload requires a recursive region contract");
          else if (p.semantics.containsProtocolOwnedFields(payload))
            fail({}, DiagnosticCode::InvalidCore,
                 "missing protocolled enum payload field contract");
        }
    }
  }
  for (const auto &type : p.semantics.types)
    if (type.kind == hir::Type::Reference &&
        (!p.semantics.supportsStoredReferenceReferent(type.element) || type.width != 1 ||
         (type.referenceAccess != Access::Shared && type.referenceAccess != Access::Unique) ||
         type.dropGlue != hir::DropGlue::Trivial ||
         type.representation != hir::Representation::Stack))
      fail({}, DiagnosticCode::InvalidCore, "invalid stored-reference representation");
  for (const auto &function : p.semantics.functions) {
    if ((function.kind != FunctionKind::Normal && function.kind != FunctionKind::Async) ||
        (function.effects.asynchronous && function.kind != FunctionKind::Async) ||
        (function.kind == FunctionKind::Normal && function.futureType != None))
      fail(function.loc, DiagnosticCode::InvalidCore,
           "unimplemented suspension semantics cannot enter Verified Core");
    if (function.transferInferencePending)
      fail(function.loc, DiagnosticCode::InvalidCore, "unresolved exclusive routing reached Core");
    std::set<RegionId> inputRegions;
    for (const auto &input : function.parameterRegions) {
      if (input.parameter >= p.semantics.symbols.size() || input.region == None ||
          !inputRegions.insert(input.region).second ||
          std::find(function.params.begin(), function.params.end(), input.parameter) ==
              function.params.end())
        fail(function.loc, DiagnosticCode::InvalidCore, "invalid input reference region identity");
      const auto &parameter = p.semantics.symbols[input.parameter];
      if (!parameter.reference || parameter.region != input.region ||
          (input.kind == BorrowKind::Shared) != (parameter.access == Access::Shared))
        fail(function.loc, DiagnosticCode::InvalidCore,
             "input region contradicts its reference capability");
    }
    auto compatibleResult = [&](TypeId input) {
      if (!function.resultBorrowedContainer)
        return p.semantics.borrowResultCompatible(input, function.result);
      for (const auto &leaf : p.semantics.borrowedReferences(function.result))
        if (p.semantics.borrowResultCompatible(input, leaf.reference.referent))
          return true;
      return false;
    };
    if (function.resultAccess != Access::Shared &&
        !(function.resultAccess == Access::Unique &&
          (containerMutableBorrowIntrinsic(function.intrinsicId) ||
           p.semantics.exclusivePayloadResult(function))))
      fail(function.loc, DiagnosticCode::InvalidCore, "unsupported exclusive result contract");
    if (function.resultBorrowedContainer
            ? function.resultAccess !=
                  (p.semantics.referenceCapability(function.result, Access::Shared)
                       ? Access::Shared
                       : Access::Unique)
            : !p.semantics.referenceCapability(function.result, function.resultAccess))
      fail(function.loc, DiagnosticCode::InvalidCore, "invalid result reference capability");
    if (!function.foreignC && (function.variadic || function.variadicDeclaration != None))
      fail(function.loc, DiagnosticCode::InvalidCore, "variadics require the public C ABI");
    if (function.foreignC && !ffi::signature(p.semantics, function))
      fail(function.loc, DiagnosticCode::InvalidCore, "invalid C ABI contract");
    if (function.external && !function.foreignC) {
      std::vector<TypeId> abi{function.result};
      for (auto id : function.params)
        abi.push_back(p.semantics.symbols.at(id).type);
      if (!function.trusted || function.owner != None ||
          function.intrinsicId == IntrinsicId::None ||
          findIntrinsic(function.name) != function.intrinsicId ||
          intrinsicSignature(function.intrinsicId, function.owner, function.result) != abi)
        fail(function.loc, DiagnosticCode::InvalidCore, "invalid trusted runtime ABI contract");
    }
    if (intrinsicInfo(function.intrinsicId).lowering == IntrinsicLowering::StringRecord &&
        !validStringIntrinsic(p.semantics, function))
      fail(function.loc, DiagnosticCode::InvalidCore, "invalid String intrinsic contract");
    bool consuming = function.owner != None &&
                     (p.semantics.types.at(function.owner).kind == hir::Type::Enum ||
                      function.intrinsicId == IntrinsicId::Box_into_inner) &&
                     !function.params.empty() &&
                     p.semantics.symbols.at(function.params[0]).receiver &&
                     function.access == Access::Own;
    if (intrinsicInfo(function.intrinsicId).lowering == IntrinsicLowering::ContainerStorage &&
        !validContainerIntrinsic(p.semantics, function))
      fail(function.loc, DiagnosticCode::InvalidCore, "invalid generic container signature");
    if (function.consumesSelf != consuming)
      fail(function.loc, DiagnosticCode::InvalidCore, "invalid consuming receiver contract");
    if (function.resultBorrowedContainer !=
        (function.resultOrigin != None && p.semantics.containsReferences(function.result)))
      fail(function.loc, DiagnosticCode::InvalidCore, "invalid borrowed-container result contract");
    if (function.resultBorrowedContainer &&
        !p.semantics.borrowedResultLayoutSupported(function.result))
      fail(function.loc, DiagnosticCode::InvalidCore,
           "unsupported borrowed result carrier/drop layout");
    if ((function.resultOrigin == None) != (function.resultRegion == None))
      fail(function.loc, DiagnosticCode::InvalidCore, "incomplete reference result contract");
    if (function.resultOrigin != None) {
      if (function.resultOrigin >= p.semantics.symbols.size() || function.external ||
          (!function.intrinsic.empty() && !containerBorrowIntrinsic(function.intrinsicId)) ||
          (function.owner != None && p.semantics.types[function.owner].protocol != None) ||
          (!function.resultPayloadSource &&
           std::find(function.params.begin(), function.params.end(), function.resultOrigin) ==
               function.params.end()))
        fail(function.loc, DiagnosticCode::InvalidCore, "invalid reference result origin");
      const auto &origin = p.semantics.symbols[function.resultOrigin];
      if (!origin.reference ||
          (!function.resultBorrowedContainer && origin.access != function.resultAccess) ||
          !compatibleResult(origin.type))
        fail(function.loc, DiagnosticCode::InvalidCore, "invalid reference result contract");
    }
    std::set<SymbolId> resultOrigins{function.resultOrigin};
    for (auto id : function.additionalResultOrigins) {
      if (!function.resultBorrowedContainer || function.resultOrigin == None ||
          id >= p.semantics.symbols.size() || !resultOrigins.insert(id).second)
        fail(function.loc, DiagnosticCode::InvalidCore, "invalid additional result origin");
      const auto &origin = p.semantics.symbols[id];
      if (!origin.reference ||
          (!function.resultBorrowedContainer && origin.access != function.resultAccess) ||
          !compatibleResult(origin.type))
        fail(function.loc, DiagnosticCode::InvalidCore,
             "invalid additional result region contract");
    }
    std::set<SymbolId> payloadParameters, payloadOrigins;
    std::set<std::pair<SymbolId, hir::ReferencePath>> payloadPositions;
    for (const auto &contract : function.payloadParameterRegions) {
      if (function.external || !function.intrinsic.empty() ||
          contract.parameter >= p.semantics.symbols.size() ||
          contract.origin >= p.semantics.symbols.size() || contract.region == None ||
          !inputRegions.insert(contract.region).second ||
          std::find(function.params.begin(), function.params.end(), contract.parameter) ==
              function.params.end() ||
          std::find(function.params.begin(), function.params.end(), contract.origin) !=
              function.params.end() ||
          !payloadPositions.insert({contract.parameter, contract.path}).second ||
          !payloadOrigins.insert(contract.origin).second)
        fail(function.loc, DiagnosticCode::InvalidCore, "invalid payload parameter contract");
      const auto &parameter = p.semantics.symbols[contract.parameter];
      const auto &origin = p.semantics.symbols[contract.origin];
      payloadParameters.insert(contract.parameter);
      const auto leaves = p.semantics.borrowedReferences(parameter.type);
      auto leaf = std::find_if(leaves.begin(), leaves.end(),
                               [&](const auto &leaf) { return leaf.path == contract.path; });

      if (parameter.reference ||
          (parameter.receiver &&
           (!p.semantics.borrowedReceiverSupported(parameter.type, parameter.access) ||
            (!p.semantics.referenceCapability(parameter.type, Access::Shared) &&
             (function.resultBorrowedContainer || function.returnsReference())))) ||
          !origin.reference || !origin.parameter || origin.receiver ||
          (origin.access != Access::Shared && origin.access != Access::Unique) ||
          origin.region != contract.region || !p.semantics.borrowedInputSupported(parameter.type) ||
          leaf == leaves.end() || leaf->reference.referent != origin.type ||
          leaf->reference.capability != origin.access)
        fail(function.loc, DiagnosticCode::InvalidCore, "invalid payload origin contract");
    }
    const auto resultTransfers = p.semantics.referenceTransfers(function);
    if (!p.semantics.referenceResultPresenceSupported(function, resultTransfers))
      fail(function.loc, DiagnosticCode::InvalidCore,
           "result presence contradicts conditional input positions");
    std::set<SymbolId> resultPayloadOrigins;
    std::set<std::pair<SymbolId, hir::ReferencePath>> resultPayloadPositions;
    for (const auto &source : function.payloadResultSources()) {
      if (!function.resultBorrowedContainer || !payloadParameters.contains(source.parameter))
        fail(function.loc, DiagnosticCode::InvalidCore, "invalid payload result source");
      const auto &path = source.path;
      auto contract = std::find_if(
          function.payloadParameterRegions.begin(), function.payloadParameterRegions.end(),
          [&](const auto &r) { return r.parameter == source.parameter && r.path == path; });
      if (contract == function.payloadParameterRegions.end() ||
          !function.acceptsResultOrigin(contract->origin) ||
          !resultPayloadPositions.insert({source.parameter, path}).second ||
          !resultPayloadOrigins.insert(contract->origin).second)
        fail(function.loc, DiagnosticCode::InvalidCore, "invalid payload result path or region");
    }
    if (function.resultPayloadSource) {
      auto source = function.resultPayloadSource->parameter;
      const auto &path = function.resultPayloadSource->path;
      auto contract = std::find_if(
          function.payloadParameterRegions.begin(), function.payloadParameterRegions.end(),
          [&](const auto &r) { return r.parameter == source && r.path == path; });
      if (contract == function.payloadParameterRegions.end() ||
          contract->origin != function.resultOrigin)
        fail(function.loc, DiagnosticCode::InvalidCore, "payload result primary origin mismatch");
    }
    for (auto id : function.additionalResultOrigins)
      if (std::find(function.params.begin(), function.params.end(), id) == function.params.end() &&
          !resultPayloadOrigins.contains(id))
        fail(function.loc, DiagnosticCode::InvalidCore, "additional origin has no input contract");
    for (auto id : function.payloadInspectionParameters) {
      if (id >= p.semantics.symbols.size() || function.external || !function.intrinsic.empty() ||
          std::find(function.params.begin(), function.params.end(), id) == function.params.end() ||
          !payloadParameters.insert(id).second)
        fail(function.loc, DiagnosticCode::InvalidCore, "invalid payload inspection contract");
      const auto &parameter = p.semantics.symbols[id];
      if (!parameter.receiver || parameter.reference || parameter.access != Access::Shared ||
          !p.semantics.opaqueBorrowedPayload(parameter.type))
        fail(function.loc, DiagnosticCode::InvalidCore, "invalid opaque payload receiver");
    }
    for (auto id : function.params)
      if (p.semantics.containsReferences(p.semantics.symbols[id].type) !=
          payloadParameters.contains(id))
        fail(function.loc, DiagnosticCode::InvalidCore, "missing payload parameter contract");
      else if (payloadParameters.contains(id) &&
               std::find(function.payloadInspectionParameters.begin(),
                         function.payloadInspectionParameters.end(),
                         id) == function.payloadInspectionParameters.end())
        for (const auto &leaf : p.semantics.borrowedReferences(p.semantics.symbols[id].type))
          if (!payloadPositions.contains({id, leaf.path}))
            fail(function.loc, DiagnosticCode::InvalidCore,
                 "missing reference position input contract");
    if (function.resultOrigin != None) {
      if (inputRegions.contains(function.resultRegion))
        fail(function.loc, DiagnosticCode::InvalidCore,
             "result region aliases an input region parameter");
      std::set<SymbolId> positionalOrigins;
      for (const auto &[path, sources] : p.semantics.referenceTransfers(function)) {
        if (sources.empty())
          fail(function.loc, DiagnosticCode::InvalidCore,
               "result reference position has no input contract");
        for (const auto &source : sources)
          positionalOrigins.insert(source.origin);
      }
      if (positionalOrigins != resultOrigins)
        fail(function.loc, DiagnosticCode::InvalidCore,
             "result origins contradict positional reference transfers");
    }
    if (function.resultOrigin == None && !function.external && function.intrinsic.empty() &&
        (p.semantics.containsReferences(function.result) ||
         p.semantics.types.at(function.result).protocol != None ||
         (!p.semantics.heap(function.result) && p.semantics.needsDrop(function.result) &&
          p.semantics.types.at(function.result).kind != hir::Type::Structure &&
          p.semantics.types.at(function.result).kind != hir::Type::Enum)))
      fail(function.loc, DiagnosticCode::InvalidCore, "unsupported result transfer contract");
  }
  if (!p.semantics.library) {
    const auto &entry = p.semantics.entryPoint;
    if (entry.function >= p.semantics.functions.size())
      fail({}, DiagnosticCode::InvalidCore, "missing semantic entrypoint");
    const auto &main = p.semantics.functions[entry.function];
    auto result = entry.signature == hir::MainSignatureKind::NoArgsUnit ? Unit : I32;
    if (!main.params.empty() || main.result != result || main.external)
      fail(main.loc, DiagnosticCode::InvalidCore,
           "entrypoint contract does not match its function");
  }
  for (const auto &f : p.functions) {
    const auto &signature = p.semantics.functions.at(f.id);
    std::map<ValueId, TypeId> values;
    std::map<ValueId, Access> references;
    for (const auto &region : signature.parameterRegions) {
      const auto &symbol = p.semantics.symbols.at(region.parameter);
      const auto reference = p.semantics.symbolReference(region.parameter);
      if (!symbol.parameter || !symbol.reference || !reference ||
          reference->region != region.region ||
          (region.kind == BorrowKind::Shared) != (reference->capability == Access::Shared))
        fail(symbol.loc, DiagnosticCode::InvalidCore, "invalid parameter-region contract");
    }
    for (auto id : signature.params) {
      const auto &symbol = p.semantics.symbols.at(id);
      size_t contracts = 0;
      for (const auto &region : signature.parameterRegions)
        contracts += region.parameter == id;
      if (contracts != (symbol.reference ? 1u : 0u))
        fail(symbol.loc, DiagnosticCode::InvalidCore,
             "missing or duplicate parameter-region contract");
    }
    for (size_t n = 0; n < f.parameters.size(); ++n)
      values.emplace(f.parameters[n], p.semantics.symbols.at(signature.params.at(n)).type);
    unsigned unsafeDepth = signature.unsafeFunction ? 1 : 0;
    std::set<uint32_t> loopDefinitions, activeLoops;
    std::function<void(const std::vector<Inst> &)> block;
    block = [&](const std::vector<Inst> &ops) {
      for (const auto &i : ops) {
        auto bad = [&]() {
          fail(i.loc, DiagnosticCode::InvalidCore, "invalid typed Core instruction");
        };
        if (!i.payloadPath.empty() && i.kind != Inst::BorrowShared &&
            i.kind != Inst::BorrowUnique && i.kind != Inst::PayloadTake)
          bad();
        for (auto value : i.args)
          if (references.contains(value) && !functionCall(i) && i.kind != Inst::ForeignCall &&
              i.kind != Inst::ProtocolTransition && i.kind != Inst::Return &&
              i.kind != Inst::BorrowShared && i.kind != Inst::BorrowUnique &&
              i.kind != Inst::RawAddress)
            bad();
        auto expect = [&](TypeId got, TypeId wanted) {
          if (got != wanted)
            bad();
        };
        auto arg = [&](size_t n) {
          if (n >= i.args.size() || !values.contains(i.args[n]))
            bad();
          return values.at(i.args[n]);
        };
        auto slot = [&]() { return p.semantics.symbols.at(i.slot).type; };
        auto q = i.kind == Inst::BorrowShared || i.kind == Inst::BorrowUnique ? Quantity::One
                 : i.kind == Inst::ProtocolTransition && i.type == Unit       ? Quantity::Zero
                 : p.semantics.linear(i.type)                                 ? Quantity::One
                                                                              : Quantity::Omega;
        if (i.quantity != q)
          bad();
        if (i.kind == Inst::Loop)
          block(i.condition);
        if (i.kind == Inst::UnsafeScope) {
          if (i.result != None || !i.args.empty() || !i.otherwise.empty() || !i.latch.empty() ||
              !i.condition.empty())
            bad();
          ++unsafeDepth;
          block(i.body);
          --unsafeDepth;
          continue;
        }
        if (!i.fieldPath.empty() && i.kind != Inst::StructInit && i.kind != Inst::RawLoad &&
            i.kind != Inst::RawStore && i.kind != Inst::FieldStore && i.kind != Inst::FieldTake &&
            i.kind != Inst::FieldDrop &&
            !(i.placeProjection == Inst::PlaceProjection::Field &&
              (i.kind == Inst::Read || i.kind == Inst::Write)) &&
            !(i.placeProjection == Inst::PlaceProjection::Field &&
              (i.kind == Inst::BorrowShared || i.kind == Inst::BorrowUnique)))
          bad();
        switch (i.kind) {
        case Inst::FutureCreate: {
          if (i.function >= p.semantics.functions.size())
            bad();
          const auto &target = p.semantics.functions[i.function];
          if (target.kind != FunctionKind::Async || i.type != target.futureType ||
              i.result == None || i.slot != None || i.args.size() != target.params.size() ||
              i.offset != 0)
            bad();
          for (size_t n = 0; n < i.args.size(); ++n)
            expect(arg(n), p.semantics.symbols.at(target.params[n]).type);
          break;
        }
        case Inst::Await:
        case Inst::BlockOn: {
          if (i.function >= p.semantics.functions.size() || i.args.size() != 1 || i.slot != None ||
              (i.kind == Inst::Await && signature.kind != FunctionKind::Async))
            bad();
          const auto &target = p.semantics.functions[i.function];
          if (target.kind != FunctionKind::Async || target.futureType == None ||
              i.type != target.result || (i.type == Unit ? i.result != None : i.result == None))
            bad();
          expect(arg(0), target.futureType);
          break;
        }
        case Inst::FunctionAddress:
          if (!i.args.empty() || i.function >= p.semantics.functions.size() ||
              !ffi::callbackSignature(p.semantics, i.type, p.semantics.functions[i.function]))
            bad();
          break;
        case Inst::ThreadRun:
          if (i.type != U64 || i.result == None || i.slot != None || i.args.size() != 1 ||
              !i.body.empty() || !i.otherwise.empty() || !i.condition.empty() || !i.latch.empty() ||
              !i.locals.empty() || !i.merges.empty() || i.resourceInput != None ||
              i.function >= p.semantics.functions.size() || i.offset >= p.semantics.types.size() ||
              !ffi::callbackSignature(p.semantics, i.offset, p.semantics.functions[i.function]))
            bad();
          expect(arg(0), U64);
          if (p.semantics.types[i.offset].element != U64 ||
              p.semantics.types[i.offset].parameters != std::vector<TypeId>{U64})
            bad();
          break;
        case Inst::IndirectForeignCall: {
          if (!unsafeDepth || !p.semantics.functionPointer(arg(0)) || i.offset != arg(0))
            bad();
          const auto &target = p.semantics.types[arg(0)];
          expect(i.type, target.element);
          if (i.args.size() != target.parameters.size() + 1)
            bad();
          for (size_t n = 0; n < target.parameters.size(); ++n)
            expect(arg(n + 1), target.parameters[n]);
          break;
        }
        case Inst::RawLoad:
        case Inst::RawStore: {
          if (!unsafeDepth || !p.semantics.rawPointer(arg(0)) || !p.semantics.cScalar(i.type) ||
              i.args.size() != (i.kind == Inst::RawLoad ? 1u : 2u))
            bad();
          const auto &pointer = p.semantics.types[arg(0)];
          if (i.kind == Inst::RawStore && !pointer.mutablePointer)
            bad();
          auto tid = pointer.element;
          for (auto index : i.fieldPath) {
            const auto &record = p.semantics.types.at(tid);
            if (record.kind != hir::Type::ForeignRecord || index >= record.fields.size())
              bad();
            tid = record.fields[index].type;
          }
          expect(tid, i.type);
          if (i.kind == Inst::RawStore)
            expect(arg(1), i.type);
          break;
        }
        case Inst::PointerCast:
          if (!unsafeDepth || i.args.size() != 1 ||
              !((p.semantics.rawPointer(arg(0)) &&
                 (p.semantics.rawPointer(i.type) || i.type == USize)) ||
                (arg(0) == USize && p.semantics.rawPointer(i.type))))
            bad();
          break;
        case Inst::RawAddress:
          if (!unsafeDepth || i.args.size() != 1 || !p.semantics.rawPointer(i.type) ||
              !scalar(arg(0)) || !p.semantics.target.littleEndian ||
              !references.contains(i.args[0]) || p.semantics.types[i.type].element != arg(0) ||
              (p.semantics.types[i.type].mutablePointer &&
               references.at(i.args[0]) != Access::Unique))
            bad();
          break;
        case Inst::UnsafeScope:
          bad();
          break;
        case Inst::StoreReference: {
          const auto stored = p.semantics.referenceInfo(i.type);
          const auto source = p.semantics.symbolReference(i.slot);
          if (!stored || stored->referent != slot() || stored->capability != i.access ||
              !p.semantics.symbols.at(i.slot).reference || !source ||
              source->capability != i.access ||
              (i.access != Access::Shared && i.access != Access::Unique))
            bad();
          break;
        }
        case Inst::ReferenceArgument: {
          expect(slot(), i.type);
          references[i.result] = i.access;
          const auto reference = p.semantics.symbolReference(i.slot);
          if (!p.semantics.symbols.at(i.slot).reference || !reference ||
              reference->capability != i.access)
            bad();
          break;
        }
        case Inst::BorrowShared:
        case Inst::BorrowUnique: {
          if (i.placeProjection != Inst::PlaceProjection::None) {
            const auto &source = p.semantics.types[slot()];
            if (!i.payloadPath.empty())
              bad();
            if (i.placeProjection == Inst::PlaceProjection::Field) {
              if (i.fieldPath.empty() || i.fieldIndex != i.fieldPath.back() || !i.args.empty())
                bad();
              auto type = slot();
              for (auto index : i.fieldPath) {
                const auto &container = p.semantics.types[type];
                if (container.kind != hir::Type::Structure || p.semantics.heap(type) ||
                    index >= container.fields.size())
                  bad();
                type = container.fields[index].type;
              }
              const auto &from = p.semantics.types[type];
              const auto &to = p.semantics.types[i.type];
              bool array = from.kind == hir::Type::Array;
              if (!(array && to.kind == hir::Type::Slice && from.element == to.element))
                expect(type, i.type);
              if (!(scalar(i.type) && i.kind == Inst::BorrowShared) && !array &&
                  !p.semantics.ownedFieldType(i.type))
                bad();
            } else {
              if (!scalar(i.type) && p.semantics.types[i.type].kind != hir::Type::Array)
                bad();
              if ((source.kind != hir::Type::Array && source.kind != hir::Type::Slice) ||
                  i.args.size() != 1)
                bad();
              expect(source.element, i.type);
              expect(arg(0), USize);
              if (references.contains(i.args[0]))
                bad();
            }
          } else if (!i.args.empty()) {
            auto access = i.kind == Inst::BorrowShared ? Access::Shared : Access::Unique;
            if (i.args.size() != 1 || !references.contains(i.args[0]) ||
                references.at(i.args[0]) != access || !i.payloadPath.empty())
              bad();
            expect(arg(0), i.type);
          }
          if (i.offset >= p.semantics.symbols.size())
            bad();
          const auto &alias = p.semantics.symbols.at(i.offset);
          const auto reference = p.semantics.symbolReference(i.offset);
          if (!alias.reference || !reference ||
              reference->capability !=
                  (i.kind == Inst::BorrowShared ? Access::Shared : Access::Unique) ||
              reference->region != i.region)
            bad();
          expect(p.semantics.symbols.at(i.offset).type, i.type);
          if (i.placeProjection != Inst::PlaceProjection::None || !i.args.empty()) {

          } else if (i.payloadPath.empty() && !p.semantics.reference(slot())) {
            const auto &from = p.semantics.types[slot()];
            const auto &to = p.semantics.types[i.type];
            if (!(from.kind == hir::Type::Array && to.kind == hir::Type::Slice &&
                  from.element == to.element))
              expect(slot(), i.type);
          } else {
            auto payload = p.semantics.referencePositionType(slot(), i.payloadPath);
            bool stored = payload && p.semantics.reference(*payload);
            if (!stored && (i.payloadPath.size() != 1 ||
                            i.payloadPath[0].kind != hir::ReferenceProjectionKind::VariantPayload))
              bad();
            if (stored && i.kind == Inst::BorrowUnique && !storedFieldReborrow(p.semantics, i) &&
                !(i.payloadPath.empty() && p.semantics.reference(slot())) &&
                (i.payloadPath.size() != 1 ||
                 i.payloadPath[0].kind != hir::ReferenceProjectionKind::VariantPayload))
              bad();
            bool found = payload &&
                         (*payload == i.type ||
                          (stored &&
                           (storedFieldReborrow(p.semantics, i) ||
                            p.semantics.types[*payload].referenceAccess ==
                                (i.kind == Inst::BorrowShared ? Access::Shared : Access::Unique)) &&
                           p.semantics.types[*payload].element == i.type));
            if (!found || p.semantics.types[i.type].protocol != None ||
                (i.kind == Inst::BorrowUnique && !p.semantics.heap(i.type) &&
                 !(stored && p.semantics.types[*payload].referenceAccess == Access::Unique)))
              bad();
          }
          break;
        }
        case Inst::RefineVariant:
          if (p.semantics.types[slot()].kind != hir::Type::Enum ||
              i.offset >= p.semantics.types[slot()].variants.size())
            bad();
          break;
        case Inst::StaticStr:
          expect(i.type, Str);
          if (!validUtf8(i.text))
            bad();
          break;
        case Inst::Length:
          expect(i.type, USize);
          if (p.semantics.types[slot()].kind != hir::Type::Array &&
              p.semantics.types[slot()].kind != hir::Type::Slice)
            bad();
          break;
        case Inst::TextPrint:
          if (p.semantics.types[slot()].representation != hir::Representation::StringRecord)
            bad();
          break;
        case Inst::Constant:
          if (!scalar(i.type))
            bad();
          break;
        case Inst::Copy:
          expect(slot(), i.type);
          if (p.semantics.linear(i.type))
            bad();
          break;
        case Inst::Move:
          expect(slot(), i.type);
          if (!p.semantics.linear(i.type))
            bad();
          break;
        case Inst::Bind:
          expect(slot(), i.type);
          expect(arg(0), i.type);
          break;
        case Inst::ArrayRepeat:
          if (p.semantics.types[i.type].kind != hir::Type::Array || i.args.size() != 1)
            bad();
          {
            auto element = p.semantics.types[i.type].element;
            while (i.text == "zero" && p.semantics.types[element].kind == hir::Type::Array)
              element = p.semantics.types[element].element;
            if (!scalar(element))
              bad();
            expect(arg(0), element);
          }
          break;
        case Inst::Array:
          if (p.semantics.types[i.type].kind != hir::Type::Array ||
              i.args.size() != p.semantics.types[i.type].count)
            bad();
          for (size_t n = 0; n < i.args.size(); ++n)
            expect(arg(n), p.semantics.types[i.type].element);
          break;
        case Inst::PayloadTake: {
          const auto &t = p.semantics.types[slot()];
          if (t.kind != hir::Type::Enum || !p.semantics.linear(i.type) ||
              p.semantics.reference(i.type) || i.offset != 0)
            bad();
          auto payload = p.semantics.payloadType(slot(), i.payloadPath);
          if (!payload || *payload != i.type)
            bad();
          break;
        }
        case Inst::FieldStore:
        case Inst::FieldTake:
        case Inst::FieldDrop: {
          auto parent = slot();
          for (auto index : i.fieldPath) {
            const auto &shape = p.semantics.types[parent];
            if (shape.kind != hir::Type::Structure || p.semantics.heap(parent) ||
                shape.protocol != None || p.semantics.containsReferences(parent) ||
                index >= shape.fields.size())
              bad();
            parent = shape.fields[index].type;
          }
          const auto &container = p.semantics.types[parent];
          if (container.kind != hir::Type::Structure || p.semantics.heap(parent) ||
              container.protocol != None || i.fieldIndex >= container.fields.size())
            bad();
          auto field = container.fields.at(i.fieldIndex).type;
          expect(field, i.type);
          if (!p.semantics.managedFieldType(field))
            bad();
          if (i.kind == Inst::FieldStore) {
            if (i.args.size() != 1)
              bad();
            expect(arg(0), field);
          } else if (!i.args.empty())
            bad();
          break;
        }
        case Inst::Read:
        case Inst::Write:
          if (i.placeProjection == Inst::PlaceProjection::Field) {
            auto parent = slot();
            for (auto index : i.fieldPath) {
              const auto &shape = p.semantics.types[parent];

              if (shape.kind != hir::Type::Structure || p.semantics.heap(parent) ||
                  index >= shape.fields.size())
                bad();
              parent = shape.fields[index].type;
            }
            const auto &container = p.semantics.types[parent];
            if (container.kind != hir::Type::Structure || p.semantics.heap(parent) ||
                i.fieldIndex >= container.fields.size() || !scalar(i.type) ||
                container.fields[i.fieldIndex].type != i.type ||
                container.fields[i.fieldIndex].offset != i.offset ||
                i.args.size() != (i.kind == Inst::Read ? 0u : 1u))
              bad();
            if (i.kind == Inst::Write)
              expect(arg(0), i.type);
            break;
          }
          if (i.args.size() == (i.kind == Inst::Read ? 1u : 2u)) {
            const auto &container = p.semantics.types[slot()];
            if (container.kind != hir::Type::Array && container.kind != hir::Type::Slice)
              bad();
            expect(i.type, container.element);
            expect(arg(i.kind == Inst::Read ? 0 : 1), USize);
            if (i.kind == Inst::Write)
              expect(arg(0), i.type);
            break;
          }
          for (auto field : p.semantics.managedFields(slot()))
            if (const auto &f = p.semantics.types[slot()].fields[field];
                i.offset >= f.offset && i.offset < f.offset + p.semantics.storageWidth(f.type))
              bad();
          if (i.offset >= p.semantics.types[slot()].width ||
              (!scalar(i.type) && i.type != RawPtr && !p.semantics.rawPointer(i.type) &&
               !p.semantics.functionPointer(i.type)))
            bad();
          if (i.kind == Inst::Write) {
            if (p.semantics.types[slot()].kind == hir::Type::Enum)
              bad();
            expect(arg(0), i.type);
          }
          break;
        case Inst::EnumConstruct: {
          auto &t = p.semantics.types[i.type];
          if (t.futureFunction != None || t.kind != hir::Type::Enum ||
              i.offset >= t.variants.size())
            bad();
          auto &v = t.variants[i.offset];
          if (i.args.size() != v.payload.size())
            bad();
          for (size_t n = 0; n < i.args.size(); ++n)
            expect(arg(n), v.payload[n]);
          break;
        }
        case Inst::StructInit: {
          const auto &type = p.semantics.types[i.type];
          if (type.kind != hir::Type::Structure || p.semantics.heap(i.type) ||
              i.args.size() != type.fields.size() || i.fieldPath.size() != type.fields.size())
            bad();
          std::set<uint32_t> fields;
          for (size_t n = 0; n < i.args.size(); ++n) {
            auto field = i.fieldPath[n];
            if (field >= type.fields.size() || !fields.insert(field).second)
              bad();
            expect(arg(n), type.fields[field].type);
          }
          break;
        }
        case Inst::Create:
          if (p.semantics.types[i.type].futureFunction != None ||
              p.semantics.types[i.type].kind != hir::Type::Structure ||
              p.semantics.dynamicContainer(i.type))
            bad();
          break;
        case Inst::Cast: {
          auto from = arg(0);
          if (!(from == i.type || (from == Char && integer(i.type)) ||
                (from == Bool && i.type == I32 && i.text == "c.default.promotion") ||
                (numeric(from) && numeric(i.type) && !(floating(from) && integer(i.type)))))
            bad();
          break;
        }
        case Inst::Binary: {
          auto operand = arg(0);
          expect(arg(1), operand);
          if (i.text == "&&" || i.text == "||") {
            expect(operand, Bool);
            expect(i.type, Bool);
          } else if (i.text == "==" || i.text == "!=" || i.text == "<" || i.text == ">" ||
                     i.text == "<=" || i.text == ">=") {
            if (!scalar(operand))
              bad();
            expect(i.type, Bool);
          } else {
            if (!numeric(operand))
              bad();
            if ((i.text == "&" || i.text == "|" || i.text == "^" || i.text == "<<" ||
                 i.text == ">>") &&
                !integer(operand))
              bad();
            expect(i.type, operand);
          }
          break;
        }
        case Inst::Unary:
          expect(i.type, arg(0));
          if (i.text == "!")
            expect(i.type, Bool);
          else if (!numeric(i.type))
            bad();
          break;
        case Inst::Loop:
          if (i.loop == None || !loopDefinitions.insert(i.loop).second || !i.otherwise.empty())
            bad();
          if (!i.unconditional)
            expect(arg(0), Bool);
          break;
        case Inst::Break:
        case Inst::Continue:

          if (!activeLoops.contains(i.loop) || i.result != None || !i.args.empty() ||
              i.type != Unit)
            bad();
          break;
        case Inst::If:
          expect(arg(0), Bool);
          break;
        case Inst::Return:
          if (auto result = p.semantics.functionReferenceResult(f.id)) {
            if (i.args.size() != 1 || !references.contains(i.args[0]) ||
                references.at(i.args[0]) != result->capability)
              bad();
          } else if (!i.args.empty() && references.contains(i.args[0]))
            bad();
          if (signature.result == Unit) {
            if (!i.args.empty())
              bad();
          } else
            expect(arg(0), signature.result);
          break;
        case Inst::ForeignCall:
        case Inst::ContainerCall:
        case Inst::InputCall:
        case Inst::StringCall:
        case Inst::HeapAlloc:
        case Inst::HeapFree:
        case Inst::HeapLoad:
        case Inst::HeapStore:
        case Inst::MemCopy:
        case Inst::MemMove:
        case Inst::Call:
        case Inst::ThreadCall:
        case Inst::ProtocolTransition: {
          if (i.function >= p.semantics.functions.size())
            bad();
          const auto &target = p.semantics.functions.at(i.function);
          if (i.kind == Inst::ThreadCall &&
              (i.slot != None || !i.body.empty() || !i.otherwise.empty() || !i.condition.empty() ||
               !i.latch.empty() || !i.locals.empty()))
            bad();
          if (target.kind == FunctionKind::Async)
            bad();
          expect(i.type, target.result);
          if ((i.kind == Inst::ForeignCall) != (target.foreignC && target.external) ||
              ((target.unsafeFunction || (target.foreignC && target.external)) && !unsafeDepth))
            bad();
          if (auto result = p.semantics.functionReferenceResult(i.function))
            references[i.result] = result->capability;
          if (i.intrinsicId != target.intrinsicId)
            bad();
          size_t skip = i.slot == None ? 0 : 1;
          if (i.args.size() + skip != target.params.size())
            bad();
          if (skip) {
            expect(slot(), p.semantics.symbols.at(target.params[0]).type);
            if (i.access != target.access)
              bad();
          }
          bool storageCall =
              intrinsicInfo(target.intrinsicId).lowering == IntrinsicLowering::ContainerStorage;
          if ((i.kind == Inst::ContainerCall) != storageCall ||
              (storageCall &&
               (i.text != target.intrinsic || !validContainerIntrinsic(p.semantics, target))))
            bad();
          if (i.kind == Inst::InputCall &&
              (intrinsicInfo(target.intrinsicId).lowering != IntrinsicLowering::InputRecord ||
               i.text != target.intrinsic))
            bad();
          if (i.kind == Inst::StringCall &&
              (!target.intrinsic.starts_with("string.") || i.text != target.intrinsic))
            bad();
          for (size_t n = 0; n < i.args.size(); ++n) {
            const auto &parameter = p.semantics.symbols.at(target.params[n + skip]);
            expect(arg(n), parameter.type);
            if (parameter.reference != references.contains(i.args[n]))
              bad();
            if (parameter.reference && references.at(i.args[n]) != parameter.access)
              bad();
          }
          break;
        }
        case Inst::Print:
          if (i.type != Unit || i.result != None || i.args.size() != 1 || i.slot != None ||
              (!scalar(arg(0)) && arg(0) != Str))
            bad();
          break;
        case Inst::Discard:
          expect(arg(0), i.type);
          break;
        default:
          break;
        }
        if (i.result != None && !values.emplace(i.result, i.type).second)
          bad();

        if (!i.body.empty() || !i.latch.empty() || !i.otherwise.empty()) {
          auto before = values;
          if (i.kind == Inst::Loop)
            activeLoops.insert(i.loop);
          block(i.body);
          if (i.kind == Inst::Loop)
            activeLoops.erase(i.loop);
          block(i.latch);
          values = before;
          block(i.otherwise);
          values = std::move(before);
        }
      }
    };
    block(f.body);
  }
}
}
