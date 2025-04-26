#pragma once
#include "isac/frontend.hpp"
#include <map>
namespace isac::macros {
using MacroId = uint32_t;
using ModuleId = uint32_t;
enum class AtSymbolKind { Macro, Intrinsic, CompilerBuiltin };
enum class BuiltinId { None, Format, Print, Println, Input, SizeOf, AlignOf };
struct AtSymbol {
  AtSymbolKind kind;
  uint32_t id;
};
BuiltinId findBuiltin(const std::string &);

struct TokenTree {
  Token token;
  char delimiter = 0;
  Token closing;
  std::vector<TokenTree> children;
};
struct Rule {
  std::vector<TokenTree> pattern, expansion;
};
struct MacroDecl {
  MacroId id;
  ModuleId module = 0;
  bool visible = true;
  std::string name;
  Loc origin;
  std::vector<Rule> rules;
};
struct Expansion {
  ExpansionId id, parent;
  MacroId macro;
  Loc call, definition;
};
struct Expanded {
  std::vector<Token> tokens;
  std::vector<MacroDecl> declarations;
  std::vector<Expansion> expansions;
};
std::vector<TokenTree> tokenTrees(const std::vector<Token> &);
Expanded expand(const std::vector<Token> &, SourceManager *sources = nullptr);
void print(const Expanded &, std::ostream &);
}
