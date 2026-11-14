#include "isac/concurrency.hpp"
#include "isac/modules.hpp"
#include <functional>
#include <stdexcept>

static bool localAllocator = false;
extern "C" bool __real__ZN4isac9execution20allocatorCrossThreadEv();
extern "C" bool __wrap__ZN4isac9execution20allocatorCrossThreadEv() {
  return !localAllocator && __real__ZN4isac9execution20allocatorCrossThreadEv();
}

int main() {
  using namespace isac;
  using namespace isac::concurrency;
  auto require = [](bool value, const char *message) {
    if (!value)
      throw std::runtime_error(message);
  };
  auto parse = [](const std::string &text) {
    return core::elaborate(resolve(modules::compile(lex(text)).ast));
  };
  auto verify = [&](const std::string &text) { return core::verify(parse(text)); };
  const std::string declarations = "struct Mixed{read:&String,owned:String,edit:&mut String,}"
                                   "struct Outer{first:Mixed,second:Mixed,}";
  auto types = verify(declarations + "fn main(){}");
  Analysis analysis(types);
  auto findType = [&](const char *name) {
    const auto &all = types.get().semantics.types;
    auto found =
        std::find_if(all.begin(), all.end(), [&](const auto &t) { return t.name == name; });
    require(found != all.end(), "missing structural fixture");
    return static_cast<TypeId>(found - all.begin());
  };
  for (auto wanted : {Capability::Send, Capability::Sync}) {
    auto assessment = analysis.type(findType("Mixed"), wanted);
    require(!assessment.structural && assessment.needsRegion && assessment.obligations.size() == 6,
            "flattened or missing leaf obligations");
    unsigned allocation = 0;
    for (const auto &obligation : assessment.obligations) {
      require(types.get().semantics.types.at(obligation.type).name == "String",
              "runtime evidence attributed to aggregate instead of leaf");
      require(!obligation.path.empty() && obligation.path[0].kind == regions::ProjectionKind::Field,
              "lost field path");
      auto field = obligation.path[0].index;
      const auto expected = field == 0 ? Capability::Sync : wanted;
      require(obligation.capability == expected,
              "reference capability lost while merging obligations");
      require(obligation.path.size() == (field == 1 ? 1u : 2u), "lost reference projection");
      if (obligation.requirement == Requirement::AllocatorContract)
        ++allocation;
    }
    require(allocation == 3, "same-kind requirements at distinct positions were deduplicated");
    auto outer = analysis.type(findType("Outer"), wanted);
    require(outer.obligations.size() == 12, "repeated nominal children lost obligations");
    unsigned left = 0, right = 0;
    for (const auto &obligation : outer.obligations) {
      require(obligation.path.size() >= 2, "outer projection not retained");
      (obligation.path[0].index == 0 ? left : right)++;
    }
    require(left == 6 && right == 6, "capability cache reused a sibling's path");
  }

  const std::string input =
      "struct View{read:&i32,edit:&mut i32,}"
      "fn work(view:View){let edit=&mut view.edit;edit=10;}"
      "fn main(){let a=1;let b=2;@thread(work(View{read:&a,edit:&mut b}));b=3;}";
  auto source = verify(input);
  unsigned rejected = 0;
  auto reject = [&](const std::function<void(core::Program &)> &mutate) {
    auto proposal = source.get();
    mutate(proposal);
    try {
      (void)core::verify(std::move(proposal));
    } catch (const Error &) {
      ++rejected;
      return;
    }
    throw std::runtime_error("joined call accepted forged regional contract");
  };
  auto worker = [](core::Program &p) -> hir::Function & {
    return *std::find_if(p.semantics.functions.begin(), p.semantics.functions.end(),
                         [](const auto &f) { return f.name == "work"; });
  };
  reject([&](auto &p) { worker(p).payloadParameterRegions.clear(); });
  reject([&](auto &p) {
    auto &regions = worker(p).payloadParameterRegions;
    regions[1].path = regions[0].path;
  });
  reject([&](auto &p) {
    auto &regions = worker(p).payloadParameterRegions;
    regions[1].region = regions[0].region;
  });

  localAllocator = true;
  (void)verify(
      "struct View{value:&String,}fn inspect(value:&String)->usize{return value.len();}"
      "fn work(view:View)->usize{return inspect(view.value);}"
      "fn main(){let text=String::from(\"shared\");print(@thread(work(View{value:&text})));}");
  (void)verify(
      "fn work(value:Option<&String>)->usize{return match value{Some(text)=>text.len(),None=>0usize,};}"
      "fn main(){let text=String::from(\"shared\");print(@thread(work(Option::Some(&text))));}");
  (void)verify(
      "struct View{value:&String,}fn reference(value:&String)->&String{return value;}"
      "fn work(view:View)->usize{let ignored=reference(view.value);let item=reference(view.value);return item.len();}"
      "fn main(){let text=String::from(\"shared\");print(@thread(work(View{value:&text})));}");
  for (const std::string &parameter : {"value:&mut String", "value:String"}) {
    const auto argument = parameter == "value:String" ? "text.move()" : "&mut text";
    try {
      (void)verify("struct View{" + parameter +
                   ",}fn work(view:View){}"
                   "fn main(){let text=String::from(\"owned\");@thread(work(View{value:" +
                   argument + "}));}");
    } catch (const Error &e) {
      require(e.diagnostic.code == DiagnosticCode::FutureFeature, "wrong provider rejection");
      ++rejected;
      continue;
    }
    throw std::runtime_error("local allocator admitted Send obligation through a carrier");
  }
  localAllocator = false;
  require(rejected == 5, "incomplete scoped carrier forgery/provider rejections");
}
