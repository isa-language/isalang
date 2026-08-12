#include "isac/backend.hpp"
#include <stdexcept>
#include <string>

using namespace isac;
using namespace isac::runtime;

int main() {

  for (auto kind : {Instruction::Alloc, Instruction::RecordAlloc}) {
    Instruction slot{};
    slot.kind = kind;
    slot.result = 0;
    slot.type = kind == Instruction::Alloc ? Type{Type::Buffer, 1, 64} : Type{Type::Pointer};
    slot.literal = 1;
    for (bool inEntry : {true, false}) {
      LIR lir;
      lir.target = selectTarget({});
      lir.values = 4;
      CFGFunction function;
      function.name = "stack_boundary";
      function.result = {Type::Scalar, 1, 64};
      function.params = {{3, {Type::Scalar, 1, 32}}};
      function.blocks = {{0, {}, {Terminator::Jump, NoValue, 1}},
                         {1, {}, {Terminator::Branch, 3, 1, 2}},
                         {2, {}, {Terminator::Return, 2}}};
      function.blocks[inEntry ? 0 : 1].instructions.push_back(slot);
      Instruction constant{};
      constant.kind = Instruction::Constant;
      constant.type = function.result;
      constant.result = 1;
      constant.literal = 42;
      Instruction store{};
      store.kind = kind == Instruction::Alloc ? Instruction::Store : Instruction::RawStore;
      store.args = {0, 1};
      Instruction load{};
      load.kind = kind == Instruction::Alloc ? Instruction::Load : Instruction::RawLoad;
      load.type = function.result;
      load.result = 2;
      load.args = {0};
      auto &body = function.blocks[1].instructions;
      body.insert(body.end(), {constant, store, load});
      lir.functions.push_back(function);
      for (auto stage : {BackendStage::LLVMDialect, BackendStage::LLVMIR}) {
        bool rejected = false;
        try {
          auto text = generate(lir, stage);
          if (text.find("alloca") == std::string::npos)
            return 1;
        } catch (const std::runtime_error &error) {
          if (std::string(error.what()).find("stack allocation outside the function entry") ==
              std::string::npos)
            throw;
          rejected = true;
        }
        if (rejected == inEntry)
          return 2;
      }
    }
  }
}
