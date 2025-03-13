#include "isac/backend.hpp"
#include "isac/erasure.hpp"
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
int main(int argc, char **argv) {
  const char *usage =
      "usage: isac file.isa [--check | --emit-tokens | --emit-ast | --emit-hir | "
      "--emit-core | --emit-erased | --emit-lir | --emit-mlir | --emit-std-mlir | "
      "--emit-llvm-dialect | --emit-llvm | -o executable [--sanitize] [--runtime-test-counters]]\n";
  if (argc == 2 && std::string(argv[1]) == "--help") {
    std::cout << usage;
    return 0;
  }
  std::string input, mode = "--check", output;
  bool selected = false;
  isac::NativeOptions nativeOptions;
  try {
    std::set<std::string> modes = {
        "--check",         "--emit-tokens",       "--emit-ast", "--emit-hir",
        "--emit-core",     "--emit-erased",       "--emit-lir", "--emit-mlir",
        "--emit-std-mlir", "--emit-llvm-dialect", "--emit-llvm"};
    for (int n = 1; n < argc; ++n) {
      std::string arg = argv[n];
      if (arg == "--sanitize") {
        nativeOptions.sanitize = true;
        continue;
      }
      if (arg == "--runtime-test-counters") {
        nativeOptions.counters = true;
        continue;
      }
      if (arg == "-o") {
        if (selected || n + 1 == argc)
          throw std::runtime_error(
              "-o requires an output path and cannot be combined with emit/check");
        selected = true;
        mode = "native";
        output = argv[++n];
      } else if (modes.contains(arg)) {
        if (selected)
          throw std::runtime_error("select one output mode");
        selected = true;
        mode = arg;
      } else if (!arg.empty() && arg[0] == '-')
        throw std::runtime_error("unknown option: " + arg);
      else {
        if (!input.empty())
          throw std::runtime_error("expected one input file");
        input = arg;
      }
    }
    if (input.empty()) {
      std::cerr << usage;
      return 1;
    }
    if ((nativeOptions.sanitize || nativeOptions.counters) && mode != "native")
      throw std::runtime_error("--sanitize/--runtime-test-counters require -o");
    std::ifstream f(input);
    if (!f)
      throw std::runtime_error("cannot open input: " + input);
    std::stringstream b;
    b << f.rdbuf();
    auto ts = isac::lex(b.str());
    if (mode == "--emit-tokens") {
      for (auto &t : ts)
        std::cout << t.loc.line << ":" << t.loc.column << " " << t.text << "\n";
      return 0;
    }
    for (auto &t : ts)
      t.loc.file = input;
    auto ast = isac::loadImports(isac::parse(ts));
    if (mode == "--emit-ast") {
      isac::printAST(ast, std::cout);
      return 0;
    }
    auto h = isac::resolve(ast);
    if (mode == "--emit-hir") {
      isac::printHIR(h, std::cout);
      return 0;
    }
    auto verified = isac::core::verify(isac::core::elaborate(std::move(h)));
    if (mode == "--check")
      return 0;
    if (mode == "--emit-core") {
      isac::core::print(verified.get(), std::cout);
      return 0;
    }
    auto erased = isac::erase(verified);
    if (mode == "--emit-erased") {
      isac::runtime::print(erased, std::cout);
      return 0;
    }
    auto lir = isac::runtime::lower(erased);
    if (mode == "--emit-lir") {
      isac::runtime::print(lir, std::cout);
      return 0;
    }
    auto stage = mode == "--emit-mlir"           ? isac::BackendStage::IsacMLIR
                 : mode == "--emit-std-mlir"     ? isac::BackendStage::StandardMLIR
                 : mode == "--emit-llvm-dialect" ? isac::BackendStage::LLVMDialect
                                                 : isac::BackendStage::LLVMIR;
    auto text = isac::generate(lir, stage, nativeOptions.sanitize);
    if (mode == "native")
      isac::compileNative(text, output, nativeOptions);
    else
      std::cout << text;
    return 0;
  } catch (const isac::Error &e) {
    std::cerr << (e.loc.file.empty() ? input : e.loc.file) << ":" << e.loc.line << ":"
              << e.loc.column << ": error[" << e.code << "]: " << e.what() << "\n";
  } catch (const std::exception &e) {
    std::cerr << "error: " << e.what() << "\n";
  }
  return 1;
}
