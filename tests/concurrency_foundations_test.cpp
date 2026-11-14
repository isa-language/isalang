#include "isac/concurrency.hpp"
#include "isac/modules.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <type_traits>

int main(int argc, char **argv) {
  using namespace isac;
  using namespace isac::concurrency;
  static_assert(!std::is_constructible_v<Analysis, core::Program &>);
  static_assert(!std::is_constructible_v<Analysis, core::Verified &&>);
  static_assert(!std::is_constructible_v<Analysis, const core::Verified &&>);
  static_assert(!std::is_constructible_v<core::Verified, core::Program>);
  if (argc != 2)
    return 1;
  auto require = [](bool condition, const char *message) {
    if (!condition)
      throw std::runtime_error(message);
  };
  auto from = [](const std::string &source) {
    return core::verify(core::elaborate(resolve(modules::compile(lex(source)).ast)));
  };
  auto load = [&](const char *name) {
    std::ifstream input(std::filesystem::path(argv[1]) / name);
    return from(std::string((std::istreambuf_iterator<char>(input)), {}));
  };
  auto findType = [&](const auto &p, const std::string &name) {
    auto type = std::find_if(p.semantics.types.begin(), p.semantics.types.end(),
                             [&](const auto &t) { return t.name == name; });
    require(type != p.semantics.types.end(), "missing fixture type");
    return static_cast<TypeId>(type - p.semantics.types.begin());
  };
  auto structural = load("valid_structural.isa");
  Analysis a(structural);
  for (TypeId type :
       {Unit, I8, I16, I32, I64, U8, U16, U32, U64, ISize, USize, F32, F64, Bool, Char})
    for (auto cap : {Capability::Send, Capability::Sync}) {
      auto result = a.type(type, cap);
      require(result.structural && !result.needsRegion && result.contracts.empty(),
              "scalar structural rule failed");
    }
  for (const auto &t : structural.get().semantics.types)
    if (t.name == "Point" || t.name.starts_with("Pair<") || t.name.starts_with("Choice<") ||
        t.kind == hir::Type::Array) {
      auto id = static_cast<TypeId>(&t - structural.get().semantics.types.data());
      require(a.type(id, Capability::Send).structural && a.type(id, Capability::Sync).structural,
              "recursive array/generic aggregate structural rule failed");
    }
  require(a.type(RawPtr, Capability::Send).contracts.contains(Requirement::ForeignContract),
          "raw address granted Send");
  try {
    (void)a.type(None, Capability::Send);
    throw std::runtime_error("invalid type query accepted");
  } catch (const Error &e) {
    require(e.code == "E9001", "wrong type query diagnostic");
  }
  try {
    (void)a.symbol(None, Capability::Send);
    throw std::runtime_error("invalid symbol query accepted");
  } catch (const Error &e) {
    require(e.code == "E9001", "wrong symbol query diagnostic");
  }
  require(a.type(Str, Capability::Send).structural && a.type(Str, Capability::Send).needsRegion,
          "str shape was mistaken for proof of a static lifetime");
  require(referentCapability(Access::Shared, Capability::Send) == Capability::Sync &&
              referentCapability(Access::Unique, Capability::Send) == Capability::Send &&
              referentCapability(Access::Shared, Capability::Sync) == Capability::Sync &&
              referentCapability(Access::Unique, Capability::Sync) == Capability::Sync,
          "reference capabilities were conflated");

  auto borrowed = load("valid_borrowed_state.isa");
  Analysis b(borrowed);
  const auto &bp = borrowed.get();
  const auto &observe = *std::find_if(bp.semantics.functions.begin(), bp.semantics.functions.end(),
                                      [](const auto &f) { return f.name == "observe"; });
  const auto &g = *std::find_if(bp.regionGraphs.begin(), bp.regionGraphs.end(),
                                [&](const auto &g) { return g.function == observe.id; });
  std::vector<SymbolId> data;
  std::set<SymbolId> locals;
  for (const auto &block : g.blocks)
    locals.insert(block.definitions.begin(), block.definitions.end());
  for (auto id : locals)
    if (bp.semantics.symbols[id].name == "data")
      data.push_back(id);
  require(data.size() == 2 && data[0] != data[1], "shadowing fixture lacks distinct identities");
  bool shared = false, unique = false, alias = false, sparse = false, observed = false,
       values = false;
  for (const auto &s : bp.semantics.symbols)
    if (s.reference && !s.parameter) {
      require(b.symbol(s.id, Capability::Send).needsRegion,
              "alias referent type hid its region obligation");
      alias = true;
    }
  for (const auto &t : bp.semantics.types)
    if (t.kind == hir::Type::Reference) {
      auto id = static_cast<TypeId>(&t - bp.semantics.types.data());
      require(b.type(id, Capability::Send).structural && b.type(id, Capability::Send).needsRegion,
              "stored-reference type lost the region obligation");
    }
  for (auto point : g.resourcePoints) {
    auto before = b.point(observe.id, {point, 0});
    auto after = b.point(observe.id, {point, 0}, Side::After);
    require(before.liveSymbols == g.blocks[point].liveIn &&
                after.liveSymbols == g.blocks[point].liveOut,
            "query substituted lexical names or late liveness");
    for (auto loan : before.loanDemands) {
      const auto &demand = g.loans.at(loan);
      require(demand.points.contains(point) && demand.place.root != data[1],
              "loan origin confused by shadowing");
      shared |= demand.capability == BorrowKind::Shared;
      unique |= demand.capability == BorrowKind::Unique;
    }
    auto transition = g.carriers.transitions.find(point);
    if (transition != g.carriers.transitions.end()) {
      require(before.carriersObserved && after.carriersObserved &&
                  before.carriers == transition->second.before &&
                  after.carriers == transition->second.after,
              "carrier observation was reconstructed incorrectly");
      observed = true;
    } else if (!g.carriers.identities.empty()) {
      require(!before.carriersObserved && !after.carriersObserved,
              "missing sparse record was treated as empty proof");
      sparse = true;
    }
    for (const auto &value : before.liveValues) {
      require(g.blocks[point].valuesIn.contains(value.value) &&
                  g.valueDefinitions.at(value.value).type == value.type,
              "live ValueId lost its rich type");
      values = true;
    }
  }
  require(shared && unique && alias && sparse && observed && values,
          "incomplete point-query coverage");
  for (auto bad : {regions::ProgramPoint{None, 0}, regions::ProgramPoint{g.entry, 1}})
    try {
      (void)b.point(observe.id, bad);
      throw std::runtime_error("invalid semantic point accepted");
    } catch (const Error &e) {
      require(e.code == "E9001", "wrong point diagnostic");
    }
  try {
    (void)b.point(None, {0, 0});
    throw std::runtime_error("invalid function query accepted");
  } catch (const Error &e) {
    require(e.code == "E9001", "wrong function query diagnostic");
  }
  bool synthetic = false;
  for (const auto &block : g.blocks)
    if (!g.resourcePoints.contains(block.id)) {
      try {
        (void)b.point(observe.id, {block.id, 0});
        throw std::runtime_error("unwitnessed CFG point accepted");
      } catch (const Error &e) {
        require(e.code == "E9001", "wrong unwitnessed point diagnostic");
      }
      synthetic = true;
      break;
    }
  require(synthetic, "missing synthetic edge fixture");

  auto runtime = load("valid_runtime_contracts.isa");
  Analysis r(runtime);
  bool heap = false, protocol = false;
  for (TypeId id = 0; id < runtime.get().semantics.types.size(); ++id) {
    const auto &type = runtime.get().semantics.types[id];
    for (auto cap : {Capability::Send, Capability::Sync}) {
      auto properties = r.type(id, cap);
      if (runtime.get().semantics.heap(id)) {
        require(!properties.structural &&
                    properties.contracts.contains(Requirement::AllocatorContract) &&
                    properties.contracts.contains(Requirement::RuntimeTypeContract),
                "heap provider was assumed thread-safe");
        heap = true;
      }
      if (type.protocol != None) {
        require(!properties.structural &&
                    properties.contracts.contains(Requirement::ProtocolContract),
                "typestate mistaken for thread permission");
        protocol = true;
      }
    }
  }
  auto owners = r.type(findType(runtime.get(), "Owners"), Capability::Send);
  require(heap && protocol && !owners.structural && !owners.witness.empty(),
          "nested blocker lost its semantic path");
  auto other = from("struct Point{x:String,}fn main(){}");
  Analysis different(other);
  require(!different.type(findType(other.get(), "Point"), Capability::Send).structural &&
              a.type(findType(structural.get(), "Point"), Capability::Send).structural,
          "capability cache leaked between programs");
  auto foreign = from(
      "extern \"C\" type Callback=extern \"C\" fn(i32)->i32; unsafe fn read(p:*const i32)->i32{return *p;}"
      "struct Handle{id:usize,}impl Handle{unsafe fn raw(shared self)->usize{return self.id;}}fn main(){}");
  Analysis f(foreign);
  for (TypeId id = 0; id < foreign.get().semantics.types.size(); ++id) {
    const auto &t = foreign.get().semantics.types[id];
    if (t.kind == hir::Type::RawPointer || t.kind == hir::Type::FunctionPointer ||
        t.name == "Handle")
      require(f.type(id, Capability::Send).contracts.contains(Requirement::ForeignContract),
              "foreign/unsafe wrapper lacked a thread contract");
  }
  for (const char *name : {"valid_loop_cleanup.isa", "valid_reference_results.isa"}) {
    auto p = load(name);
    Analysis query(p);
    for (const auto &graph : p.get().regionGraphs)
      for (auto point : graph.resourcePoints)
        (void)query.point(graph.function, {point, 0});
  }

  for (unsigned mode = 0; mode < 3; ++mode) {
    auto ast = modules::compile(lex("fn main(){}")).ast;
    auto target = std::find_if(ast.functions.begin(), ast.functions.end(),
                               [](const auto &fn) { return fn.name == "main"; });
    if (mode == 0) {
      target->kind = FunctionKind::Async;
      target->unsafeFunction = true;
    }
    if (mode == 1)
      target->effects.asynchronous = true;
    if (mode == 2)
      target->kind = static_cast<FunctionKind>(99);
    try {
      (void)resolve(ast);
      throw std::runtime_error("unimplemented async AST accepted");
    } catch (const Error &e) {
      require(e.code == "E8006", "wrong async AST gate diagnostic");
    }
  }
  for (unsigned mode = 0; mode < 3; ++mode) {
    auto raw = core::elaborate(resolve(parse(lex("fn main(){}"))));
    if (mode == 0)
      raw.semantics.functions[0].kind = FunctionKind::Async;
    if (mode == 1)
      raw.semantics.functions[0].effects.asynchronous = true;
    if (mode == 2)
      raw.semantics.functions[0].kind = static_cast<FunctionKind>(99);
    try {
      (void)core::verify(std::move(raw));
      throw std::runtime_error("unimplemented async Core accepted");
    } catch (const Error &e) {
      require(e.code == "E9001", "wrong async Core gate diagnostic");
    }
  }
  std::ostringstream dump;
  concurrency::print(borrowed, dump);
  require(dump.str().find("queries only; execution requires checked contracts") !=
                  std::string::npos &&
              dump.str().find("sparse; not empty") != std::string::npos,
          "report claimed a permission or hid missing observations");
}
