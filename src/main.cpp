#include "isac/backend.hpp"
#include "isac/erasure.hpp"
#include "isac/liveness.hpp"
#include "isac/modules.hpp"
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
int main(int argc, char **argv) {
  const char *usage =
      "usage: isac file.isa [--check | --emit-tokens | --emit-ast | --emit-hir | "
      "--emit-expanded | --emit-imports | --emit-regions | --emit-region-constraints | --emit-core "
      "| --emit-erased "
      "| --emit-lir | "
      "--emit-liveness | --emit-mlir "
      "| --emit-std-mlir | "
      "--emit-llvm-dialect | --emit-llvm | --emit-asm | --emit-obj | -o executable "
      "[--sanitize] [--runtime-test-counters]]\nOptions: --target=TRIPLE --cpu=CPU "
      "--features=+FEATURE -O0/-O1/-O2/-O3/-Os/-Oz --print-target --color=auto/always/never\n";
  if (argc == 2 && std::string(argv[1]) == "--help") {
    std::cout << usage;
    return 0;
  }
  std::string input, mode = "--check", output;
  bool selected = false;
  isac::NativeOptions nativeOptions;
  isac::CompilerOptions options;
  bool printTarget = false;
  isac::SourceManager sources;
  isac::ColorMode color = isac::ColorMode::Auto;
  try {
    std::set<std::string> modes = {"--emit-region-constraints",
                                   "--emit-regions",
                                   "--check",
                                   "--emit-expanded",
                                   "--emit-imports",
                                   "--emit-liveness",
                                   "--emit-tokens",
                                   "--emit-ast",
                                   "--emit-hir",
                                   "--emit-core",
                                   "--emit-erased",
                                   "--emit-lir",
                                   "--emit-mlir",
                                   "--emit-std-mlir",
                                   "--emit-llvm-dialect",
                                   "--emit-llvm",
                                   "--emit-asm",
                                   "--emit-obj"};
    for (int n = 1; n < argc; ++n) {
      std::string arg = argv[n];
      if (arg.starts_with("--color=")) {
        auto c = arg.substr(8);
        if (c != "auto" && c != "always" && c != "never")
          throw std::runtime_error("invalid color mode");
        color = c == "always"  ? isac::ColorMode::Always
                : c == "never" ? isac::ColorMode::Never
                               : isac::ColorMode::Auto;
        continue;
      }
      if (arg == "--print-target") {
        printTarget = true;
        continue;
      }
      if (arg.starts_with("--target=")) {
        options.target.triple = arg.substr(9);
        continue;
      }
      if (arg.starts_with("--cpu=")) {
        options.target.cpu = arg.substr(6);
        continue;
      }
      if (arg.starts_with("--features=")) {
        options.target.features = arg.substr(11);
        continue;
      }
      if (arg == "-O0" || arg == "-O1" || arg == "-O2" || arg == "-O3" || arg == "-Os" ||
          arg == "-Oz") {
        options.optimization = arg == "-O0"   ? isac::OptimizationLevel::O0
                               : arg == "-O1" ? isac::OptimizationLevel::O1
                               : arg == "-O2" ? isac::OptimizationLevel::O2
                               : arg == "-O3" ? isac::OptimizationLevel::O3
                               : arg == "-Os" ? isac::OptimizationLevel::Os
                                              : isac::OptimizationLevel::Oz;
        continue;
      }
      if (arg == "--sanitize") {
        nativeOptions.sanitize = true;
        continue;
      }
      if (arg == "--runtime-test-counters") {
        nativeOptions.counters = true;
        continue;
      }
      if (arg == "-o") {
        if (!output.empty() || n + 1 == argc)
          throw std::runtime_error("-o requires one output path");
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
    nativeOptions.target = isac::selectTarget(options.target);
    nativeOptions.optimization = options.optimization;
    if (printTarget) {
      std::cout << nativeOptions.target.triple << "\n";
      return 0;
    }
    if (!selected && !output.empty())
      mode = "native";
    if ((mode == "--emit-obj" || mode == "--emit-asm") && output.empty())
      throw std::runtime_error("object/assembly emission requires -o path");
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
    auto ts = isac::lex(b.str(), sources.add(input, b.str()));
    if (mode == "--emit-tokens") {
      for (auto &t : ts)
        std::cout << t.loc.line << ":" << t.loc.column << " " << t.text << "\n";
      return 0;
    }
    for (auto &t : ts)
      t.loc.file = input;
    auto compilation = isac::modules::compile(ts, &sources);
    if (mode == "--emit-imports") {
      isac::modules::print(compilation.modules, std::cout);
      return 0;
    }
    auto &expanded = compilation.expanded;
    if (mode == "--emit-expanded") {
      isac::macros::print(expanded, std::cout);
      return 0;
    }
    auto ast = std::move(compilation.ast);
    if (mode == "--emit-ast") {
      isac::printAST(ast, std::cout);
      return 0;
    }
    auto h = isac::resolve(ast, nativeOptions.target);
    for (const auto &warning : h.warnings)
      isac::TerminalRenderer(sources, color).render(warning, std::cerr);
    if (mode == "--emit-hir") {
      isac::printHIR(h, std::cout);
      return 0;
    }
    auto verified = isac::core::verify(isac::core::elaborate(std::move(h)));
    if (mode == "--check")
      return 0;
    if (mode == "--emit-region-constraints") {
      isac::regions::printConstraints(verified.get(), std::cout);
      return 0;
    }
    if (mode == "--emit-regions") {
      isac::regions::print(verified.get(), std::cout);
      return 0;
    }
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
    if (mode == "--emit-liveness") {
      isac::runtime::printLiveness(lir, std::cout);
      return 0;
    }
    if (mode == "--emit-lir") {
      isac::runtime::print(lir, std::cout);
      return 0;
    }
    auto stage = mode == "--emit-mlir"           ? isac::BackendStage::IsacMLIR
                 : mode == "--emit-std-mlir"     ? isac::BackendStage::StandardMLIR
                 : mode == "--emit-llvm-dialect" ? isac::BackendStage::LLVMDialect
                                                 : isac::BackendStage::LLVMIR;
    auto text = isac::generate(lir, stage, nativeOptions.sanitize);
    if (stage == isac::BackendStage::LLVMIR)
      text = isac::optimizeLLVM(text, nativeOptions.target, options.optimization);
    if (mode == "--emit-obj" || mode == "--emit-asm")
      isac::emitMachine(text, output, nativeOptions.target, options.optimization,
                        mode == "--emit-asm");
    else if (mode == "native")
      isac::compileNative(text, output, nativeOptions);
    else if (!output.empty()) {
      std::ofstream emitted(output);
      emitted << text;
      if (!emitted)
        throw std::runtime_error("cannot write output");
    } else
      std::cout << text;
    return 0;
  } catch (const isac::Error &e) {
    isac::TerminalRenderer(sources, color).render(e.diagnostic, std::cerr);
    for (auto &d : e.additional)
      isac::TerminalRenderer(sources, color).render(d, std::cerr);
  } catch (const std::exception &e) {
    auto d =
        isac::DiagnosticEngine().error(isac::DiagnosticCode::BackendError, {}, e.what()).build();
    isac::TerminalRenderer(sources, color).render(d, std::cerr);
  }
  return 1;
}
