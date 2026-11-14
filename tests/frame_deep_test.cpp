#include "isac/frames.hpp"
#include "isac/modules.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>

int main(int argc, char **argv) {
  using namespace isac;
  using I = core::Inst;
  using Fields = std::map<uint32_t, core::OwnedFieldState>;
  auto require = [](bool yes, const char *why) {
    if (!yes)
      throw std::runtime_error(why);
  };
  if (argc != 2)
    return 1;
  unsigned rejected = 0, deepIdentity = 0;
  for (const char *name : {"recursive.isa", "empty_child.isa", "early.isa"}) {
    std::ifstream input(std::filesystem::path(argv[1]) / "frame_deep" / name);
    std::string text((std::istreambuf_iterator<char>(input)), {});
    auto source = core::verify(core::elaborate(resolve(modules::compile(lex(text)).ast)), {true});
    const auto &original = source.get();
    const std::string boundary = std::string(name) == "empty_child.isa" ? "second" : "first";
    const core::CaptureState *point = nullptr;
    for (const auto &f : original.functions)
      for (const auto &i : f.body)
        if (i.kind == I::Call && original.semantics.functions[i.function].name == boundary)
          for (const auto &s : original.captureStates)
            if (s.function == f.id && s.side == core::StateSide::Before &&
                s.point.block == i.semanticPoint)
              point = &s;
    require(point, "missing deep capture witness");
    for (const auto &f : original.functions) {
      const auto &g = *std::find_if(original.regionGraphs.begin(), original.regionGraphs.end(),
                                    [&](const auto &g) { return g.function == f.id; });
      std::function<void(const std::vector<I> &)> inspect = [&](const auto &body) {
        for (const auto &i : body) {
          inspect(i.body);
          inspect(i.otherwise);
          inspect(i.condition);
          inspect(i.latch);
          if (i.kind != I::FieldTake || i.fieldPath.empty() || !original.semantics.heap(i.type))
            continue;
          storage::Location field{storage::Location::Symbol, i.slot};
          for (auto n : i.fieldPath)
            field.path.push_back({hir::ReferenceProjectionKind::Field, n});
          field.path.push_back({hir::ReferenceProjectionKind::Field, i.fieldIndex});
          const auto &transition = g.storage.transitions.at(i.semanticPoint);
          require(transition.before.at(field) ==
                      transition.after.at({storage::Location::Value, i.result}),
                  "deep field extraction lost or invented an allocation identity");
          require(!transition.after.contains(field),
                  "deep field extraction retained old storage ownership");
          ++deepIdentity;
        }
      };
      inspect(f.body);
    }
    auto prepared = frames::projectCapture(source, *point);
    require(prepared.has_value(), "deep partial tree was not normalized");
    const auto &p = prepared->source.get();
    const auto &after = prepared->witness();
    require(!frames::projectCapture(prepared->source, after), "normalized tree is still partial");
    std::function<bool(TypeId, const Fields &)> complete = [&](TypeId type, const Fields &fields) {
      for (auto index : p.semantics.managedFields(type))
        if (!fields.contains(index) ||
            !complete(p.semantics.types[type].fields[index].type, fields.at(index).fields))
          return false;
      return true;
    };
    std::vector<storage::Location> expected, actual;
    std::function<void(storage::Location, TypeId, const Fields &)> expand =
        [&](auto place, auto type, const Fields &fields) {
          if (complete(type, fields)) {
            expected.push_back(place);
            return;
          }
          const auto &shape = p.semantics.types[type];
          for (size_t n = shape.fields.size(); n-- > 0;)
            if (fields.contains(n)) {
              auto child = place;
              child.path.push_back({hir::ReferenceProjectionKind::Field, static_cast<uint32_t>(n)});
              expand(std::move(child), shape.fields[n].type, fields.at(n).fields);
            }
        };
    for (const auto &drop : point->cleanup)
      if (p.semantics.linear(drop.type)) {
        const auto &r = *std::find_if(point->resources.begin(), point->resources.end(),
                                      [&](const auto &r) { return r.location == drop.location; });
        expand(drop.location, drop.type, r.fields);
      }
    std::map<SymbolId, storage::Location> origins;
    for (const auto &t : prepared->transfers)
      origins.emplace(t.staging, t.source);
    for (const auto &drop : after.cleanup)
      if (p.semantics.linear(drop.type))
        actual.push_back(origins.at(drop.location.id));
    require(expected == actual, "deep staging changed recursive destruction chronology");
    const auto function = [&](core::Program &copy) -> core::Function & {
      return *std::find_if(copy.functions.begin(), copy.functions.end(),
                           [&](const auto &f) { return f.id == point->function; });
    };
    auto copy = p;
    const auto &body = function(copy).body;
    size_t take = body.size(), store = body.size(), skeleton = body.size(), drop = body.size();
    for (size_t n = 0; n < body.size(); ++n) {
      const auto &i = body[n];
      if (n < prepared->boundaryIndex && i.kind == I::FieldTake && !i.fieldPath.empty())
        take = n;
      if (n < prepared->boundaryIndex && i.kind == I::Drop && take < n && i.slot == body[take].slot)
        drop = n;
      if (n > prepared->boundaryIndex && i.kind == I::FieldStore) {
        if (!i.fieldPath.empty())
          store = n;
        if (p.semantics.types[i.type].kind == hir::Type::Structure && !p.semantics.heap(i.type))
          skeleton = n;
      }
    }
    require(take < body.size() && store < body.size() && skeleton < body.size() &&
                drop < body.size(),
            "missing ordinary recursive transfer operations");
    const auto *leaf = &prepared->transfers.front();
    for (const auto &t : prepared->transfers)
      if (p.semantics.linear(t.type) && !t.source.path.empty())
        leaf = &t;
    auto reject = [&](auto mutate) {
      auto bad = p;
      mutate(function(bad).body, bad);
      try {
        (void)core::verify(std::move(bad));
        throw std::runtime_error("invalid deep Core was accepted");
      } catch (const Error &) {
        ++rejected;
      }
    };
    reject([&](auto &b, auto &) { b[take].fieldPath.push_back(999); });
    reject([&](auto &b, auto &) { b[take].fieldPath.clear(); });
    reject([&](auto &b, auto &bad) {
      auto again = b[take];
      again.result = bad.values++;
      b.insert(b.begin() + take + 1, again);
    });
    reject([&](auto &b, auto &) { b[store].fieldPath.push_back(999); });
    reject([&](auto &b, auto &) { b.erase(b.begin() + skeleton); });
    reject([&](auto &b, auto &) { b.erase(b.begin() + drop); });
    reject([&](auto &b, auto &) { b[prepared->boundaryIndex].locals = {leaf->staging}; });

    auto forged = p;
    function(forged).body[skeleton].storedFieldPlan = {};
    auto rechecked = core::verify(std::move(forged), {true});
    require(!function(copy).body[skeleton].storedFieldPlan.fields.empty(),
            "missing partial transfer plan");
    const auto &checkedBody =
        std::find_if(rechecked.get().functions.begin(), rechecked.get().functions.end(),
                     [&](const auto &f) { return f.id == point->function; })
            ->body;
    require(checkedBody[skeleton].storedFieldPlan == body[skeleton].storedFieldPlan,
            "partial transfer proof was trusted instead of recomputed");
  }
  require(rejected == 21 && deepIdentity >= 3, "missing recursive proof coverage");
}
