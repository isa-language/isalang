#include "isac/frames.hpp"
#include "isac/erasure.hpp"
#include "isac/modules.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <type_traits>

int main(int argc, char **argv) {
  using namespace isac;
  namespace f = frames;
  static_assert(!std::is_constructible_v<f::Verified, f::Program>);
  if (argc != 2)
    return 1;
  auto require = [](bool ok, const char *why) {
    if (!ok)
      throw std::runtime_error(why);
  };
  auto load = [&](const char *name) {
    std::ifstream in(std::filesystem::path(argv[1]) / name);
    std::string text((std::istreambuf_iterator<char>(in)), {});
    require(!text.empty(), "fixture missing");
    return core::verify(core::elaborate(resolve(modules::compile(lex(text)).ast)), {true});
  };
  auto erased = [](const auto &p) {
    std::ostringstream out;
    runtime::print(erase(p), out);
    return out.str();
  };
  auto core = load("frame_construction/valid_owned.isa");
  const auto before = erased(core);
  const core::CaptureState *witness = nullptr;
  for (const auto &s : core.get().captureStates)
    if (s.cleanupValid) {
      bool one = false, two = false;
      for (const auto &r : s.resources)
        if (r.location.kind == storage::Location::Symbol) {
          const auto &name = core.get().semantics.symbols[r.location.id].name;
          one |= name == "one";
          two |= name == "two";
        }
      if (one && two) {
        witness = &s;
        break;
      }
    }
  require(witness, "no state with two owned values");
  auto checked = f::build(core, *witness);
  const auto &good = checked.get();
  require(checked.origin().function == witness->function &&
              checked.origin().point.block == witness->point.block &&
              checked.origin().side == witness->side &&
              checked.origin().entryState == witness->entryState,
          "frame proof lost its source boundary/entry alternative");
  require(good.slots.size() >= 3, "fixture must include owned and Copy inputs");
  require(checked.constructionStates().size() == good.slots.size() + 1, "missing partial states");
  bool copy = false, move = false;
  for (const auto &slot : good.slots) {
    copy |= slot.mode == core::CaptureMode::Copy;
    move |= slot.mode == core::CaptureMode::Move;
    const auto &content = checked.content(slot.id);
    require(content.type == slot.type && content.mode == slot.mode, "frame lost source typing");
    require(checked.drop(slot.id).has_value() == (slot.mode == core::CaptureMode::Move),
            "destruction obligation erased/duplicated");
  }
  require(copy && move, "capture capabilities missing");
  for (size_t n = 0; n < checked.constructionStates().size(); ++n) {
    const auto &s = checked.constructionStates()[n];
    require(s.initialized.size() == n && s.staged.size() + n == good.slots.size(),
            "partial initialization not conserved");
    for (auto slot : s.staged)
      require(!s.initialized.contains(slot), "input exists in both staging and frame");
  }
  unsigned corruptions = 0;
  auto reject = [&](auto change) {
    auto program = good;
    change(program);
    try {
      (void)f::verify(core, *witness, std::move(program));
      throw std::runtime_error("invalid frame IR accepted");
    } catch (const Error &e) {
      require(e.code == "E9001", "wrong frame corruption diagnostic");
      ++corruptions;
    }
  };
  reject([](auto &p) { p.slots[0].id = None; });
  reject([](auto &p) { p.slots[0].type = Bool; });
  reject([](auto &p) { p.slots[0].source.id = None; });
  reject([](auto &p) { p.slots[0].source = p.slots[1].source; });
  reject([](auto &p) { p.slots[0].mode = core::CaptureMode::Copy; });
  reject([](auto &p) { p.slots.pop_back(); });
  reject([](auto &p) { p.construct[0].kind = f::Operation::AcquireCopy; });
  reject([](auto &p) { p.construct.insert(p.construct.begin(), p.construct.front()); });
  reject([](auto &p) { p.construct[0].slot = None; });
  reject([](auto &p) { p.construct[0].kind = f::Operation::Initialize; });
  reject([](auto &p) { p.construct.erase(p.construct.begin()); });
  reject([](auto &p) { p.construct.pop_back(); });
  reject([](auto &p) { p.construct.push_back({f::Operation::Initialize, 0}); });
  reject([](auto &p) { p.construct.back().slot = 0; });
  reject([](auto &p) { p.construct[p.slots.size() + 1].slot = p.construct[p.slots.size()].slot; });
  reject([](auto &p) { p.cleanup.pop_back(); });
  reject([](auto &p) { p.cleanup[0].prefix = p.cleanup[1].prefix; });
  reject([](auto &p) { p.cleanup[0].prefix = None; });
  reject([](auto &p) { p.cleanup[0].body[0].kind = f::Operation::DropSlot; });
  reject([](auto &p) { p.cleanup.back().body[0].kind = f::Operation::DropStaged; });
  reject(
      [](auto &p) { p.cleanup[0].body.insert(p.cleanup[0].body.begin(), p.cleanup[0].body[0]); });
  reject([](auto &p) { p.cleanup[0].body.erase(p.cleanup[0].body.begin()); });
  reject([](auto &p) { p.cleanup[0].body.pop_back(); });
  reject([](auto &p) { p.cleanup[0].body.push_back({f::Operation::Finish}); });
  reject([](auto &p) { p.cleanup[0].body[0].kind = f::Operation::AcquireMove; });
  reject([](auto &p) { std::swap(p.cleanup[0].body[0], p.cleanup[0].body[1]); });
  require(corruptions == 26, "corruption coverage changed");

  auto permuted = good;
  auto n = permuted.slots.size();
  std::swap(permuted.construct[n], permuted.construct[n + 1]);
  for (auto &edge : permuted.cleanup) {
    std::set<f::SlotId> initialized;
    for (uint32_t k = 0; k < edge.prefix; ++k)
      initialized.insert(permuted.construct[n + k].slot);
    for (auto &op : edge.body)
      if (op.kind != f::Operation::Finish)
        op.kind = initialized.contains(op.slot) ? f::Operation::DropSlot : f::Operation::DropStaged;
  }
  (void)f::verify(core, *witness, std::move(permuted));
  auto foreign = *witness;
  try {
    (void)f::build(core, foreign);
    throw std::runtime_error("copied witness accepted");
  } catch (const Error &e) {
    require(e.code == "E9001", "wrong foreign witness error");
  }
  auto other = load("frame_construction/valid_owned.isa");
  try {
    (void)f::build(other, *witness);
    throw std::runtime_error("cross-program witness accepted");
  } catch (const Error &e) {
    require(e.code == "E9001", "wrong cross-program witness error");
  }
  require(erased(core) == before, "frame verification modified the original continuation");

  auto protocol = load("frame_construction/valid_protocol.isa");
  bool finalField = false;
  for (const auto &s : protocol.get().captureStates)
    if (s.cleanupValid) {
      auto frame = f::build(protocol, s);
      for (const auto &slot : frame.get().slots)
        for (const auto &[index, field] : frame.content(slot.id).fields) {
          (void)index;
          finalField |= field.state != None && !field.allocation;
        }
    }
  require(finalField, "frame reset nested protocol state/allocation to its initial state");
  bool partialGate = false, payloadGate = false, borrowGate = false;
  for (const char *name :
       {"capture_states/valid_owned_states.isa", "capture_states/valid_payload_states.isa",
        "capture_states/valid_span_contracts.isa",
        "capture_states/valid_stored_reference_gate.isa"}) {
    auto blocked = load(name);
    bool rejection = false;
    for (const auto &s : blocked.get().captureStates) {
      try {
        (void)f::build(blocked, s);
      } catch (const Error &e) {
        require(e.code != "E9001", "valid Core caused an invalid frame invariant");
        rejection = true;
        for (const auto &r : s.resources) {
          partialGate |= s.cleanupValid && !r.absentFields.empty() && e.code == "E8006";
          payloadGate |= s.cleanupValid && !r.movedPayloads.empty() && e.code == "E8006";
          borrowGate |= !s.cleanupValid && (!r.carriers.empty() || r.callerOwned || r.root != None);
        }
      }
    }
    require(rejection, "unsupported frame case bypassed its gate");
  }
  require(partialGate && payloadGate && borrowGate,
          "unsupported source gates were tested only at terminal edges");
  auto empty =
      core::verify(core::elaborate(resolve(modules::compile(lex("fn main() {}")).ast)), {true});
  bool zero = false;
  for (const auto &s : empty.get().captureStates)
    if (s.cleanupValid) {
      auto frame = f::build(empty, s);
      zero |= frame.get().slots.empty() && frame.constructionStates().size() == 1;
    }
  require(zero, "empty frame requires invented state/allocation");
}
