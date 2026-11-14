#include "isac/regions.hpp"
#include <algorithm>
#include <deque>

namespace isac::hir {
namespace {
using Sources = std::set<ReferenceSource>;
using Value = std::map<ReferencePath, Sources>;
using Bindings = std::map<uint32_t, Value>;
const Value &facts(const Bindings &bindings, uint32_t id) {
  static const Value empty;
  auto found = bindings.find(id);
  return found == bindings.end() ? empty : found->second;
}
void bind(Bindings &bindings, uint32_t id, Value value) {

  if (value.empty())
    bindings.erase(id);
  else
    bindings.insert_or_assign(id, std::move(value));
}

using RoutingValueId = uint32_t;
struct State {
  Bindings symbols, values;
  bool operator==(const State &) const = default;
};
struct Place {
  SymbolId root;
  ReferencePath path;
};
void merge(Value &to, const Value &from) {
  for (const auto &[path, sources] : from)
    to[path].insert(sources.begin(), sources.end());
}
void merge(Bindings &to, const Bindings &from) {
  for (const auto &[id, value] : from)
    merge(to[id], value);
}
void merge(State &to, const State &from) {
  merge(to.symbols, from.symbols);
  merge(to.values, from.values);
}
bool within(const ReferencePath &path, const ReferencePath &base) {
  return path.size() >= base.size() && std::equal(base.begin(), base.end(), path.begin());
}
Value extract(const Value &value, const ReferencePath &base) {
  Value result;
  for (const auto &[path, sources] : value)
    if (within(path, base))
      result[{path.begin() + base.size(), path.end()}] = sources;
  return result;
}
void erase(Value &value, const ReferencePath &base) {
  std::erase_if(value, [&](const auto &entry) { return within(entry.first, base); });
}
void store(Value &to, const ReferencePath &base, const Value &from) {
  erase(to, base);
  for (const auto &[path, sources] : from) {
    auto target = base;
    target.insert(target.end(), path.begin(), path.end());
    to[target] = sources;
  }
}
std::optional<ReferencePath> fieldPosition(const Program &p, const Expr &e) {
  if (!e.payloadPath.empty())
    return e.payloadPath;
  const auto &shape = p.types.at(e.args.at(0).type);
  if (shape.kind != Type::Structure)
    return {};
  auto field = std::find_if(shape.fields.begin(), shape.fields.end(),
                            [&](const auto &f) { return f.offset == e.ref; });
  if (field == shape.fields.end())
    return {};

  return ReferencePath{
      {ReferenceProjectionKind::Field, static_cast<uint32_t>(field - shape.fields.begin())}};
}
std::optional<Place> place(const Program &p, const Expr &e) {
  if (e.kind == Expr::Variable)
    return Place{e.ref, {}};
  if (e.kind != Expr::Field || e.args.empty())
    return {};
  auto path = fieldPosition(p, e);
  if (!path)
    return {};
  auto root = place(p, e.args.front());
  if (root)
    root->path.insert(root->path.end(), path->begin(), path->end());
  return root;
}

struct Node {
  enum Kind { Evaluate, Bind, EndAlias, Refine, Return, Edge, Exit } kind = Edge;
  regions::Terminator terminator = regions::Terminator::Branch;
  const Expr *expression = nullptr;
  SymbolId symbol = None;
  VariantId variant = None;
  RoutingValueId result = None;
  std::vector<RoutingValueId> inputs;
  std::vector<regions::BasicBlockId> successors;
};
struct Graph {
  std::vector<Node> nodes;
  regions::BasicBlockId entry = None, exit = None;
};
class GraphBuilder {
  const Program &p;
  Graph graph;
  RoutingValueId nextValue = 0;
  struct Loop {
    regions::BasicBlockId exit, latch;
  };
  std::map<uint32_t, Loop> loops;
  uint32_t add(Node node) {
    auto id = static_cast<uint32_t>(graph.nodes.size());
    graph.nodes.push_back(std::move(node));
    return id;
  }
  uint32_t edge(uint32_t next, regions::Terminator kind = regions::Terminator::Branch) {
    Node n;
    n.terminator = kind;
    n.successors = {next};
    return add(std::move(n));
  }
  std::vector<uint32_t> choices(const Expr &condition, uint32_t yes, uint32_t no) {

    if (condition.kind == Expr::Constant && condition.type == Bool) {
      if (condition.text == "1")
        return {yes};
      if (condition.text == "0")
        return {no};
    }
    return {yes, no};
  }
  uint32_t expression(const Expr &e, RoutingValueId result, uint32_t next) {
    if (e.kind == Expr::Block || e.kind == Expr::Match)
      return block(e.body, expression(e.args.at(0), result, next));
    Node n;
    n.kind = Node::Evaluate;
    n.expression = &e;
    n.result = result;
    n.successors = {next};
    for (size_t k = 0; k < e.args.size(); ++k)
      n.inputs.push_back(nextValue++);
    auto inputs = n.inputs;
    auto entry = add(std::move(n));
    if (e.kind == Expr::Binary && e.operatorClass == Expr::OperatorClass::Logical) {
      auto right = expression(e.args.at(1), inputs.at(1), entry);
      Node test;
      test.terminator = regions::Terminator::CondBranch;
      test.successors =
          e.text == "&&" ? choices(e.args[0], right, entry) : choices(e.args[0], entry, right);
      return expression(e.args[0], inputs.at(0), add(std::move(test)));
    }
    for (size_t k = e.args.size(); k > 0; --k)
      entry = expression(e.args[k - 1], inputs[k - 1], entry);
    return entry;
  }
  uint32_t condition(const Expr &e, uint32_t yes, uint32_t no) {
    Node n;
    n.terminator = regions::Terminator::CondBranch;
    n.successors = choices(e, yes, no);
    auto branch = add(std::move(n));
    return expression(e, nextValue++, branch);
  }
  uint32_t statement(const Stmt &s, uint32_t next) {
    if (s.kind == Stmt::If)
      return condition(s.expr, block(s.body, next), block(s.otherwise, next));
    if (s.kind == Stmt::Scope)
      return block(s.body, next);
    if (s.kind == Stmt::Loop) {
      auto header = add(Node{}), latch = add(Node{});
      loops[s.loop] = {next, latch};
      auto backedge = edge(header, regions::Terminator::LoopBackedge);
      auto latchEntry = block(s.latch, backedge);
      graph.nodes[latch].successors = {latchEntry};
      auto body = block(s.body, latch);
      auto test = s.unconditional ? body : condition(s.expr, body, next);
      graph.nodes[header].successors = {test};
      loops.erase(s.loop);
      return header;
    }
    if (s.kind == Stmt::Break || s.kind == Stmt::Continue) {
      const auto &loop = loops.at(s.loop);
      return edge(s.kind == Stmt::Break ? loop.exit : loop.latch);
    }
    if (s.kind == Stmt::Match) {

      auto entry = next;
      for (size_t k = s.body.size(); k > 0; --k) {
        const auto &arm = s.body[k - 1];
        auto body = block(arm.body, next);
        if (arm.variant != None) {
          Node fact;
          fact.kind = Node::Refine;
          fact.symbol = s.symbol;
          fact.variant = arm.variant;
          fact.successors = {body};
          body = add(std::move(fact));
        }
        entry = arm.catchall || k == s.body.size() ? body : condition(arm.expr, body, entry);
      }
      return entry;
    }
    if (s.kind == Stmt::BorrowEnd) {
      if (p.symbols[s.symbol].inferredRegion && !s.unconditional)
        return next;
      Node n;
      n.kind = Node::EndAlias;
      n.symbol = s.symbol;
      n.successors = {next};
      return add(std::move(n));
    }
    if (s.kind == Stmt::Let || s.kind == Stmt::BorrowStart || s.kind == Stmt::Borrow) {
      auto after = next;
      if (s.kind == Stmt::Borrow) {
        Node end;
        end.kind = Node::EndAlias;
        end.symbol = s.symbol;
        end.successors = {next};
        after = block(s.body, add(std::move(end)));
      }
      auto value = nextValue++;
      Node bind;
      bind.kind = Node::Bind;
      bind.symbol = s.symbol;
      bind.inputs = {value};
      bind.successors = {after};
      return expression(s.expr, value, add(std::move(bind)));
    }
    if (s.kind == Stmt::Return) {
      auto value = nextValue++;
      Node n;
      n.kind = Node::Return;
      n.terminator = regions::Terminator::Return;
      n.inputs = {value};
      return expression(s.expr, value, add(std::move(n)));
    }
    if (s.kind == Stmt::Expression)
      return expression(s.expr, nextValue++, next);
    fail(s.loc, DiagnosticCode::InvalidCore, "unknown typed statement in reference routing CFG");
  }
  uint32_t block(const std::vector<Stmt> &body, uint32_t next) {
    for (auto it = body.rbegin(); it != body.rend(); ++it)
      next = statement(*it, next);
    return next;
  }

public:
  GraphBuilder(const Program &p) : p(p) {}
  Graph build(const Function &f) {
    Node exit;
    exit.kind = Node::Exit;
    exit.terminator = regions::Terminator::Unreachable;
    graph.exit = add(std::move(exit));
    graph.entry = block(f.body, graph.exit);
    return std::move(graph);
  }
};

class Routing {
  const Program &p;
  const Function &function;
  bool known = true;
  Value unknown(TypeId type) {
    Value value;
    for (const auto &leaf : p.borrowedReferences(type))
      value[leaf.path].insert({None, {}, None});
    return value;
  }
  Value evaluate(const Expr &e, const std::vector<Value> &args, State &state) {
    switch (e.kind) {
    case Expr::Variable:
      return facts(state.symbols, e.ref);
    case Expr::Field:
      if (auto path = fieldPosition(p, e))
        return extract(args.at(0), *path);
      return unknown(e.type);
    case Expr::Move: {
      if (auto source = place(p, e.args.at(0))) {
        auto value = facts(state.symbols, source->root);
        erase(value, source->path);
        bind(state.symbols, source->root, std::move(value));
      } else
        known = false;
      return args.at(0);
    }
    case Expr::Drop:
      if (auto source = place(p, e.args.at(0))) {
        auto value = facts(state.symbols, source->root);
        erase(value, source->path);
        bind(state.symbols, source->root, std::move(value));
      } else
        known = false;
      return {};
    case Expr::StructInit:
    case Expr::EnumConstruct: {
      Value result;
      for (size_t k = 0; k < args.size(); ++k)
        store(result,
              e.kind == Expr::StructInit
                  ? ReferencePath{{ReferenceProjectionKind::Field, e.fieldPath.at(k)}}
                  : payloadPosition(e.ref, k),
              args[k]);
      return result;
    }
    case Expr::StoreReference:

      return args.at(0);
    case Expr::Borrow:
    case Expr::PayloadTake: {
      auto result = extract(args.at(0), e.payloadPath);
      if (e.kind == Expr::Borrow && e.payloadPath.empty() && !p.reference(e.args.at(0).type) &&
          !p.containsReferences(e.args.at(0).type)) {
        auto source = place(p, e.args.at(0));
        if (!source || !p.symbols.at(source->root).reference)

          result[{}].insert({None, {}, None});
      }
      bool take = e.kind == Expr::PayloadTake;
      if (!e.payloadPath.empty() &&
          e.payloadPath.front().kind == ReferenceProjectionKind::VariantPayload) {
        auto type = p.referencePositionType(e.args.at(0).type, e.payloadPath);
        take |= type && p.reference(*type) && p.types[*type].referenceAccess == Access::Unique;
      }
      if (take) {
        if (auto source = place(p, e.args.at(0))) {
          auto path = source->path;
          path.insert(path.end(), e.payloadPath.begin(), e.payloadPath.end());
          auto value = facts(state.symbols, source->root);
          erase(value, path);
          bind(state.symbols, source->root, std::move(value));
        } else
          known = false;
      }
      return result;
    }
    case Expr::Assign: {
      if (auto target = place(p, e.args.at(0))) {
        const auto &symbol = p.symbols.at(target->root);
        if (symbol.reference) {
          if (p.containsReferences(symbol.type))
            known = false;
        } else {
          auto value = facts(state.symbols, target->root);
          store(value, target->path, args.at(1));
          bind(state.symbols, target->root, std::move(value));
        }
      } else
        known = false;
      return {};
    }
    case Expr::Call:
    case Expr::Method: {
      const auto &callee = p.functions.at(e.ref);
      if (callee.transferInferencePending) {
        known = false;
        return {};
      }
      Value result;
      for (const auto &[path, sources] : p.referenceTransfers(callee))
        for (const auto &source : sources) {
          auto index = std::find(callee.params.begin(), callee.params.end(), source.parameter) -
                       callee.params.begin();
          if (index >= static_cast<decltype(index)>(args.size())) {
            known = false;
            continue;
          }
          auto actual = extract(args[index], source.path);
          if (auto root = actual.find({}); root != actual.end())
            result[path].insert(root->second.begin(), root->second.end());
        }
      return result;
    }
    case Expr::Unsafe:
      known = false;
      return unknown(e.type);
    default:
      return unknown(e.type);
    }
  }
  State transfer(const Node &node, State state) {
    if (node.kind == Node::Evaluate) {
      std::vector<Value> args;
      for (auto input : node.inputs)
        args.push_back(facts(state.values, input));
      bind(state.values, node.result, evaluate(*node.expression, args, state));
    } else if (node.kind == Node::Bind)
      bind(state.symbols, node.symbol, facts(state.values, node.inputs.at(0)));
    else if (node.kind == Node::EndAlias)
      state.symbols.erase(node.symbol);
    else if (node.kind == Node::Refine) {
      auto value = facts(state.symbols, node.symbol);
      std::erase_if(value, [&](const auto &entry) {
        return !entry.first.empty() &&
               entry.first.front().kind == ReferenceProjectionKind::VariantPayload &&
               entry.first.front().variant != node.variant;
      });
      bind(state.symbols, node.symbol, std::move(value));
    }
    return state;
  }

public:
  Routing(const Program &p, const Function &f) : p(p), function(f) {}
  std::optional<ReferenceTransfers> run() {
    const auto graph = GraphBuilder(p).build(function);
    State entry;
    for (const auto &input : function.payloadParameterRegions)
      entry.symbols[input.parameter][input.path].insert(
          {input.parameter, input.path, input.origin, ReferenceSourceKind::Carrier});
    for (const auto &input : function.parameterRegions)
      entry.symbols[input.parameter][{}].insert({input.parameter, {}, input.parameter});
    std::vector<State> in(graph.nodes.size()), out(graph.nodes.size());
    std::vector<bool> reached(graph.nodes.size()), queued(graph.nodes.size());
    in[graph.entry] = std::move(entry);
    std::deque<uint32_t> work{graph.entry};
    queued[graph.entry] = true;
    while (!work.empty()) {
      auto point = work.front();
      work.pop_front();
      queued[point] = false;
      auto next = transfer(graph.nodes[point], in[point]);

      merge(next, out[point]);
      if (reached[point] && next == out[point])
        continue;
      out[point] = std::move(next);
      reached[point] = true;
      for (auto successor : graph.nodes[point].successors) {
        merge(in[successor], out[point]);
        if (!queued[successor]) {
          queued[successor] = true;
          work.push_back(successor);
        }
      }
    }
    if (!known || reached[graph.exit])
      return {};
    Value returned;
    bool hasReturn = false;
    for (size_t k = 0; k < graph.nodes.size(); ++k)
      if (reached[k] && graph.nodes[k].kind == Node::Return) {
        merge(returned, facts(in[k].values, graph.nodes[k].inputs.at(0)));
        hasReturn = true;
      }
    if (!hasReturn)
      return {};
    ReferenceTransfers selected;
    std::set<ReferenceSource> used;
    for (const auto &[path, candidates] : p.referenceTransferCandidates(function)) {
      auto found = returned.find(path);
      if (found == returned.end() || found->second.size() != 1 ||
          !candidates.contains(*found->second.begin()) ||
          (p.referenceResultAccess(function, path) == Access::Unique &&
           !used.insert(*found->second.begin()).second))
        return {};
      selected[path] = found->second;
    }
    return selected;
  }
};
}
void inferReferenceTransfers(Program &p) {
  bool changed;
  do {
    changed = false;
    for (auto &f : p.functions)
      if (f.transferInferencePending) {
        auto selected = Routing(p, f).run();
        if (!selected)
          continue;
        std::set<ReferenceSource> sources;
        for (const auto &[path, inputs] : *selected)
          sources.insert(inputs.begin(), inputs.end());
        f.resultOrigin = None;
        f.resultPayloadSource.reset();
        f.additionalResultOrigins.clear();
        f.additionalPayloadSources.clear();
        for (const auto &source : sources) {
          if (f.resultOrigin == None) {
            f.resultOrigin = source.origin;
            f.resultPayloadSource = PayloadResultSource{source.parameter, source.path};
          } else {
            f.additionalResultOrigins.push_back(source.origin);
            f.additionalPayloadSources.push_back({source.parameter, source.path});
          }
        }
        f.inferredTransfers = std::move(*selected);
        f.transferInferencePending = false;
        if (!p.exclusivePayloadResult(f))
          fail(f.loc, DiagnosticCode::EscapingView,
               "inferred exclusive routing is not an affine owned-input contract");
        changed = true;
      }
  } while (changed);
  for (const auto &f : p.functions)
    if (f.transferInferencePending)
      DiagnosticEngine()
          .error(DiagnosticCode::EscapingView, f.loc,
                 "cannot infer one exclusive input origin for every result position")
          .note(
              "all returns must agree on sources per position; exclusive sources cannot be duplicated")
          .help(
              "transfer each output from one input; unresolved calls, alias effects or path-dependent routes need additional analysis")
          .emit();
}
}
