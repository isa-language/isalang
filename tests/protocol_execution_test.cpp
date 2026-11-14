#include "isac/concurrency.hpp"
#include "isac/execution.hpp"
#include "isac/modules.hpp"
#include <stdexcept>

int main() {
  using namespace isac;
  using I = core::Inst;
  auto require = [](bool value, const char *message) {
    if (!value)
      throw std::runtime_error(message);
  };
  auto parse = [](const std::string &text) {
    return core::elaborate(resolve(modules::compile(lex(text)).ast));
  };
  auto source = core::verify(
      parse("protocol State{initial Idle;final Idle;state Idle{inspect(shared self);}}"
            "struct Token:State{value:i32,}impl Token{fn inspect(shared self){let n=self.value;}}"
            "fn work(value:Token){}fn main(){@thread(work(Token::new()));}"));
  auto &sem = source.get().semantics;
  auto token = std::find_if(sem.types.begin(), sem.types.end(),
                            [](const auto &t) { return t.name == "Token"; }) -
               sem.types.begin();
  require(sem.ownedFieldType(token) && !sem.needsDrop(token),
          "protocol responsibility confused with an allocation destructor");
  concurrency::Analysis analysis(source);
  require(analysis.type(token, concurrency::Capability::Send)
              .contracts.contains(concurrency::Requirement::ProtocolContract),
          "structural query granted a protocol execution permission");
  auto proposal = source.get();
  for (auto &f : proposal.semantics.functions)
    f.effects = {};
  bool inserted = false;
  for (auto &f : proposal.functions)
    if (proposal.semantics.functions[f.id].owner == token) {
      I op{};
      op.kind = I::UnsafeScope;
      op.type = Unit;
      f.body.insert(f.body.begin(), op);
      inserted = true;
    }
  require(inserted, "missing unused protocol method");
  bool rejected = false;
  try {
    (void)core::verify(std::move(proposal));
  } catch (const Error &) {
    rejected = true;
  }
  require(rejected, "protocol method graph was trusted after deleting effect flags");
  auto opened = core::verify(parse(
      "protocol State{initial Busy;final Idle;state Busy{finish(self)->Idle;}state Idle{}}"
      "struct Token:State{value:i32,}impl Token{fn finish(self){self.value=1;}}fn work(value:Token){value.finish();}"
      "fn main(){@thread(work(Token::new()));}"));
  auto id = std::find_if(opened.get().semantics.types.begin(), opened.get().semantics.types.end(),
                         [](const auto &t) { return t.name == "Token"; }) -
            opened.get().semantics.types.begin();
  require(!execution::closedStorage(opened.get().semantics, id) &&
              execution::closedStorage(opened.get().semantics, id,
                                       execution::StorageHorizon::JoinedTransfer),
          "cancellation and joined transfer horizons were conflated");
}
