#pragma once
#include "isac/macros.hpp"
#include <ostream>
namespace isac::modules {
struct Dependency {
  uint32_t target;
  Loc origin;
  std::string alias;
  bool qualifiedOnly = false;
};
struct Module {
  uint32_t id;
  std::string path;
  bool trusted;
  std::vector<Token> tokens;
  std::vector<Dependency> dependencies;
};
struct Compilation {
  ast::Program ast;
  macros::Expanded expanded;
  std::vector<Module> modules;
};
Compilation compile(const std::vector<Token> &, SourceManager *sources = nullptr);
void print(const std::vector<Module> &, std::ostream &);
}
