#include "isac/runtime.hpp"
#include "isac/diagnostics.hpp"
#include <functional>
namespace isac::runtime {
namespace {
class Lowerer {
  CFGFunction f;
  struct LoopTarget {
    uint32_t id, exit, latch;
    bool broken = false;
  };
  std::vector<LoopTarget> loops;
  uint32_t newBlock() {
    uint32_t n = f.blocks.size();
    f.blocks.push_back({n, {}, {Terminator::Return}});
    return n;
  }
  bool sequence(const std::vector<Instruction> &seq, uint32_t &current) {
    for (auto &i : seq) {
      if (i.kind == Instruction::Break || i.kind == Instruction::Continue) {
        auto &target = loops.back();
        if (i.kind == Instruction::Break)
          target.broken = true;
        f.blocks[current].end = {Terminator::Jump, NoValue,
                                 i.kind == Instruction::Break ? target.exit : target.latch};
        return true;
      }
      if (i.kind == Instruction::Loop) {
        auto header = newBlock(), body = newBlock(), latch = newBlock(), exit = newBlock();
        f.blocks[current].end = {Terminator::Jump, NoValue, header};
        auto head = header;
        sequence(i.condition, head);
        f.blocks[head].end = i.unconditional
                                 ? Terminator{Terminator::Jump, NoValue, body}
                                 : Terminator{Terminator::Branch, i.args[0], body, exit};
        loops.push_back({i.loop, exit, latch});
        if (!sequence(i.body, body))
          f.blocks[body].end = {Terminator::Jump, NoValue, latch};
        auto step = latch;
        sequence(i.latch, step);
        f.blocks[step].end = {Terminator::Jump, NoValue, header};
        auto broken = loops.back().broken;
        loops.pop_back();
        current = exit;
        if (i.unconditional && !broken) {
          f.blocks[exit].end = {Terminator::Unreachable};
          return true;
        }
        continue;
      }
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

    std::vector<uint32_t> order, mapping(f.blocks.size(), UINT32_MAX);
    std::function<void(uint32_t)> visit = [&](uint32_t id) {
      if (mapping[id] != UINT32_MAX)
        return;
      mapping[id] = order.size();
      order.push_back(id);
      auto t = f.blocks[id].end;
      if (t.kind == Terminator::Jump || t.kind == Terminator::Branch)
        visit(t.first);
      if (t.kind == Terminator::Branch)
        visit(t.second);
    };
    visit(0);
    std::vector<Block> reachable;
    for (auto id : order) {
      auto b = std::move(f.blocks[id]);
      b.id = mapping[id];
      if (b.end.kind == Terminator::Jump || b.end.kind == Terminator::Branch)
        b.end.first = mapping[b.end.first];
      if (b.end.kind == Terminator::Branch)
        b.end.second = mapping[b.end.second];
      reachable.push_back(std::move(b));
    }
    f.blocks = std::move(reachable);

    std::vector<Instruction> slots;
    for (auto &b : f.blocks)
      for (auto it = b.instructions.begin(); it != b.instructions.end();) {
        if ((it->kind == Instruction::Alloc || it->kind == Instruction::RecordAlloc)) {
          slots.push_back(std::move(*it));
          it = b.instructions.erase(it);
        } else
          ++it;
      }
    f.blocks[0].instructions.insert(f.blocks[0].instructions.begin(), slots.begin(), slots.end());
    return std::move(f);
  }
};
void inst(const Instruction &i, std::ostream &o, int d) {
  static const char *names[] = {
      "constant", "alloc",  "load",       "store", "call",     "binary",     "unary",
      "if",       "return", "heap.alloc", "free",  "load.ptr", "store.ptr",  "memcpy",
      "memmove",  "cast",   "loop",       "break", "continue", "static_str", "record.alloc"};
  o << std::string(d, ' ');
  if (i.result != NoValue)
    o << "%" << i.result << " = ";
  o << names[i.kind];
  if (i.kind == Instruction::Alloc)
    o << " buffer[" << i.type.width << "]";
  if (i.kind == Instruction::Constant && i.type.kind == Type::Float)
    o << " " << i.real;
  else if (i.kind == Instruction::Constant)
    o << " "
      << (i.type.kind == Type::Scalar ? std::to_string(static_cast<int32_t>(i.literal))
                                      : std::to_string(i.literal));
  if (!i.callee.empty())
    o << " " << (i.kind == Instruction::StaticStr ? quoteText(i.callee) : i.callee);
  for (auto a : i.args)
    o << " %" << a;
  if (i.kind == Instruction::Load || i.kind == Instruction::Store)
    o << " offset=" << i.offset;
  o << "\n";
  for (auto &x : i.condition)
    inst(x, o, d + 2);
  for (auto &x : i.body)
    inst(x, o, d + 2);
  if (!i.latch.empty()) {
    o << std::string(d, ' ') << "latch\n";
    for (auto &x : i.latch)
      inst(x, o, d + 2);
  }
  if (!i.otherwise.empty()) {
    o << std::string(d, ' ') << "else\n";
    for (auto &x : i.otherwise)
      inst(x, o, d + 2);
  }
}
}
LIR lower(const Erased &e) {
  LIR p;
  p.target = e.target;
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
      if (t.kind == Terminator::Unreachable)
        o << "unreachable";
      else if (t.kind == Terminator::Return) {
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
