#pragma once

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>
namespace isac::runtime {
using Value = uint32_t;
inline constexpr Value NoValue = UINT32_MAX;
struct Type {
  enum Kind { Void, Scalar, Buffer, Size, Pointer } kind = Void;
  uint32_t width = 0;
  unsigned elementBits = 32;
  bool operator==(const Type &) const = default;
};
struct Instruction {
  enum Kind {
    Constant,
    Alloc,
    Load,
    Store,
    Call,
    Binary,
    Unary,
    If,
    Return,
    HeapAlloc,
    Free,
    RawLoad,
    RawStore,
    MemCopy,
    MemMove
  } kind;
  Value result = NoValue;
  Type type;
  std::vector<Value> args;
  uint64_t literal = 0;
  bool unsignedOperands = false;
  uint32_t offset = 0;
  std::string callee;
  std::vector<Instruction> body, otherwise;
};
struct Parameter {
  Value value;
  Type type;
};
struct Function {
  bool external = false;
  std::string name;
  Type result;
  std::vector<Parameter> params;
  std::vector<Instruction> body;
};
struct Erased {
  std::vector<Function> functions;
  uint32_t values = 0;
};
struct Terminator {
  enum Kind { Jump, Branch, Return } kind;
  Value value = NoValue;
  uint32_t first = 0, second = 0;
};
struct Block {
  uint32_t id;
  std::vector<Instruction> instructions;
  Terminator end{Terminator::Return};
};
struct CFGFunction {
  bool external = false;
  std::string name;
  Type result;
  std::vector<Parameter> params;
  std::vector<Block> blocks;
};
struct LIR {
  std::vector<CFGFunction> functions;
  uint32_t values = 0;
};
LIR lower(const Erased &);
void print(const Erased &, std::ostream &);
void print(const LIR &, std::ostream &);
}
