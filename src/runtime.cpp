#include "isac/runtime.hpp"
namespace isac::runtime {
namespace {
class Lowerer {
  CFGFunction f;
  uint32_t newBlock() {
    uint32_t n = f.blocks.size();
    f.blocks.push_back({n, {}, {Terminator::Return}});
    return n;
  }
  bool sequence(const std::vector<Instruction> &seq, uint32_t &current) {
    for (auto &i : seq) {
      if (i.kind == Instruction::Return) {
        f.blocks[current].end = {Terminator::Return, i.args.empty() ? NoValue : i.args[0]};
        return true;
      }
      if (i.kind == Instruction::If) {
        auto left = newBlock(), right = newBlock();
        f.blocks[current].end = {Terminator::Branch, i.args[0], left, right};
        bool l = sequence(i.body, left), r = sequence(i.otherwise, right);
        if (l && r)
          return true;
        auto join = newBlock();
        if (!l)
          f.blocks[left].end = {Terminator::Jump, NoValue, join};
        if (!r)
          f.blocks[right].end = {Terminator::Jump, NoValue, join};
        current = join;
      } else
        f.blocks[current].instructions.push_back(i);
    }
    return false;
  }

public:
  CFGFunction run(const Function &input) {
    f = {input.external, input.name, input.result, input.params, {}};
    if (input.external)
      return std::move(f);
    auto entry = newBlock();
    sequence(input.body, entry);
    return std::move(f);
  }
};
void inst(const Instruction &i, std::ostream &o, int d) {
  static const char *names[] = {"constant", "alloc",    "load",      "store",  "call",
                                "binary",   "unary",    "if",        "return", "heap.alloc",
                                "free",     "load.ptr", "store.ptr", "memcpy", "memmove"};
  o << std::string(d, ' ');
  if (i.result != NoValue)
    o << "%" << i.result << " = ";
  o << names[i.kind];
  if (i.kind == Instruction::Alloc)
    o << " buffer[" << i.type.width << "]";
  if (i.kind == Instruction::Constant)
    o << " "
      << (i.type.kind == Type::Scalar ? std::to_string(static_cast<int32_t>(i.literal))
                                      : std::to_string(i.literal));
  if (!i.callee.empty())
    o << " " << i.callee;
  for (auto a : i.args)
    o << " %" << a;
  if (i.kind == Instruction::Load || i.kind == Instruction::Store)
    o << " offset=" << i.offset;
  o << "\n";
  for (auto &x : i.body)
    inst(x, o, d + 2);
  if (!i.otherwise.empty()) {
    o << std::string(d, ' ') << "else\n";
    for (auto &x : i.otherwise)
      inst(x, o, d + 2);
  }
}
}
LIR lower(const Erased &e) {
  LIR p;
  p.values = e.values;
  for (auto &f : e.functions)
    p.functions.push_back(Lowerer().run(f));
  return p;
}
void print(const Erased &p, std::ostream &o) {
  o << "Erased IR (runtime data only)\n";
  for (auto &f : p.functions) {
    o << (f.external ? "extern fn " : "fn ") << f.name << "\n";
    for (auto &i : f.body)
      inst(i, o, 2);
  }
}
void print(const LIR &p, std::ostream &o) {
  o << "Lowered IR\n";
  for (auto &f : p.functions) {
    o << (f.external ? "extern fn " : "fn ") << f.name << "\n";
    for (auto &b : f.blocks) {
      o << "  ^bb" << b.id << ":\n";
      for (auto &i : b.instructions)
        inst(i, o, 4);
      auto &t = b.end;
      o << "    ";
      if (t.kind == Terminator::Return) {
        o << "return";
        if (t.value != NoValue)
          o << " %" << t.value;
      } else if (t.kind == Terminator::Jump)
        o << "branch ^bb" << t.first;
      else
        o << "branch %" << t.value << " ^bb" << t.first << " ^bb" << t.second;
      o << "\n";
    }
  }
}
}
