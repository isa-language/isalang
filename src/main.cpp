#include "isac/backend.hpp"
#include "isac/concurrency.hpp"
#include "isac/erasure.hpp"
#include "isac/execution.hpp"
#include "isac/async.hpp"
#include "isac/frames.hpp"
#include "isac/liveness.hpp"
#include "isac/modules.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
namespace {
class PassTimer {
  bool enabled;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

public:
  explicit PassTimer(bool enabled) : enabled(enabled) {}
  void mark(const char *name) {
    auto end = std::chrono::steady_clock::now();
    if (enabled)
      std::cerr << "time: " << name << " "
                << std::chrono::duration<double, std::milli>(end - start).count() << " ms\n";
    start = end;
  }
};
}
int main(int argc, char **argv) {
  const char *usage =
      "usage: isac file.isa [--check | --emit-tokens | --emit-ast | --emit-hir | "
      "--emit-expanded | --emit-imports | --emit-regions | --emit-region-constraints | --emit-concurrency | --emit-captures | --emit-frames | --emit-core | --emit-async-core | --emit-state-machine "
      "| --emit-erased "
      "| --emit-lir | "
      "--emit-liveness | --emit-mlir "
      "| --emit-std-mlir | "
      "--emit-llvm-dialect | --emit-llvm | --emit-asm | --emit-obj | -o executable "
      "[--sanitize] [--runtime-test-counters]]\nOptions: --target=TRIPLE --cpu=CPU "
      "--library --link-object PATH -lNAME -LPATH --emit-c-bridge\n"
      "--features=+FEATURE -O0/-O1/-O2/-O3/-Os/-Oz --print-target --time-passes "
      "--frame-roundtrip [--frame-heap] [--frame-outline] [--frame-state] [--frame-step-before=FIRST,NEXT,...] [--frame-project-fields] [--frame-state-exit=ready/checkpoint[:N]/completed] [--frame-construction-exit=N | --frame-destroy-if=BOOL] "
      "--thread-call=FUNCTION (experimental scoped integer worker) "
      "--defer-call=FUNCTION [--defer-before=FIRST,NEXT,...] (experimental local segments) --color=auto/always/never\n";
  if (argc == 2 && std::string(argv[1]) == "--help") {
    std::cout << usage;
    return 0;
  }
  std::string input, mode = "--check", output;
  bool selected = false;
  isac::NativeOptions nativeOptions;
  isac::CompilerOptions options;
  bool printTarget = false, library = false, timePasses = false;
  bool frameRoundTrip = false;
  isac::frames::RoundTripOptions frameOptions;
  std::string frameDestroyIf;
  std::string frameStepBefore;
  std::string threadCall;
  std::string deferCall, deferBefore;
  isac::SourceManager sources;
  isac::ColorMode color = isac::ColorMode::Auto;
  try {
    std::set<std::string> modes = {"--emit-c-bridge",
                                   "--emit-region-constraints",
                                   "--emit-async-core",
                                   "--emit-state-machine",
                                   "--emit-regions",
                                   "--emit-concurrency",
                                   "--emit-captures",
                                   "--emit-frames",
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
      if (arg.starts_with("--defer-call=")) {
        deferCall = arg.substr(std::string("--defer-call=").size());
        if (deferCall.empty())
          throw std::runtime_error("deferred call requires a function name");
        continue;
      }
      if (arg.starts_with("--defer-before=")) {
        deferBefore = arg.substr(std::string("--defer-before=").size());
        if (deferBefore.empty())
          throw std::runtime_error("deferred boundary requires a function name");
        continue;
      }
      if (arg.starts_with("--thread-call=")) {
        threadCall = arg.substr(std::string("--thread-call=").size());
        if (threadCall.empty())
          throw std::runtime_error("thread worker requires a function name");
        continue;
      }
      if (arg == "--frame-roundtrip") {
        frameRoundTrip = true;
        continue;
      }
      if (arg == "--frame-heap") {
        frameOptions.heap = true;
        continue;
      }
      if (arg == "--frame-outline") {
        frameOptions.outline = true;
        continue;
      }
      if (arg == "--frame-state") {
        frameOptions.stateful = true;
        continue;
      }
      if (arg == "--frame-project-fields") {
        frameOptions.projectFields = true;
        continue;
      }
      if (arg.starts_with("--frame-state-exit=")) {
        auto phase = arg.substr(std::string("--frame-state-exit=").size());
        if (phase == "ready")
          frameOptions.stateExit = isac::frames::RoundTripOptions::StateExit::Ready;
        else if (phase == "completed")
          frameOptions.stateExit = isac::frames::RoundTripOptions::StateExit::Completed;
        else if (phase == "checkpoint")
          frameOptions.stateExit = isac::frames::RoundTripOptions::StateExit::Checkpoint;
        else if (phase.starts_with("checkpoint:")) {
          auto index = phase.substr(std::string("checkpoint:").size());
          if (index.empty() || index.find_first_not_of("0123456789") != std::string::npos ||
              std::stoull(index) == 0 || std::stoull(index) >= isac::None)
            throw std::runtime_error("invalid frame checkpoint exit index");
          frameOptions.stateExit = isac::frames::RoundTripOptions::StateExit::Checkpoint;
          frameOptions.checkpointIndex = static_cast<uint32_t>(std::stoull(index));
        } else
          throw std::runtime_error(
              "invalid frame state exit; expected ready, checkpoint or completed");
        continue;
      }
      if (arg.starts_with("--frame-step-before=")) {
        frameStepBefore = arg.substr(std::string("--frame-step-before=").size());
        if (frameStepBefore.empty())
          throw std::runtime_error("frame step requires a function name");
        continue;
      }
      if (arg.starts_with("--frame-destroy-if=")) {
        frameDestroyIf = arg.substr(std::string("--frame-destroy-if=").size());
        if (frameDestroyIf.empty())
          throw std::runtime_error("frame dispatch requires a bool symbol name");
        continue;
      }
      if (arg.starts_with("--frame-construction-exit=")) {
        auto text = arg.substr(std::string("--frame-construction-exit=").size());
        if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos ||
            std::stoull(text) >= isac::None)
          throw std::runtime_error("invalid frame construction prefix");
        frameOptions.constructionExit = static_cast<uint32_t>(std::stoull(text));
        continue;
      }
      if (arg == "--time-passes") {
        timePasses = true;
        continue;
      }
      if (arg.starts_with("--color=")) {
        auto c = arg.substr(8);
        if (c != "auto" && c != "always" && c != "never")
          throw std::runtime_error("invalid color mode");
        color = c == "always"  ? isac::ColorMode::Always
                : c == "never" ? isac::ColorMode::Never
                               : isac::ColorMode::Auto;
        continue;
      }
      if (arg == "--library") {
        library = true;
        continue;
      }
      if (arg == "--link-object" || arg == "-L" || arg == "-l") {
        if (n + 1 == argc)
          throw std::runtime_error(arg + " requires a value");
        auto value = std::string(argv[++n]);
        if (arg == "--link-object")
          nativeOptions.linkInputs.push_back(value);
        else if (arg == "-L")
          nativeOptions.libraryPaths.push_back(value);
        else
          nativeOptions.libraries.push_back(value);
        continue;
      }
      if (arg.starts_with("-l") && arg.size() > 2) {
        nativeOptions.libraries.push_back(arg.substr(2));
        continue;
      }
      if (arg.starts_with("-L") && arg.size() > 2) {
        nativeOptions.libraryPaths.push_back(arg.substr(2));
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
    if (frameOptions.constructionExit && !frameRoundTrip)
      throw std::runtime_error("--frame-construction-exit requires --frame-roundtrip");
    if (!deferBefore.empty() && deferCall.empty())
      throw std::runtime_error("--defer-before requires --defer-call");
    if (frameOptions.heap && !frameRoundTrip)
      throw std::runtime_error("--frame-heap requires --frame-roundtrip");
    if (!frameDestroyIf.empty() &&
        (!frameRoundTrip || !frameOptions.heap || frameOptions.constructionExit))
      throw std::runtime_error(
          "--frame-destroy-if requires --frame-roundtrip --frame-heap and excludes construction-prefix exits");
    if (frameOptions.outline &&
        (!frameRoundTrip || !frameOptions.heap || frameOptions.constructionExit))
      throw std::runtime_error(
          "--frame-outline requires --frame-roundtrip --frame-heap and excludes construction-prefix exits");
    if (frameOptions.stateful && (!frameRoundTrip || !frameOptions.heap || !frameOptions.outline ||
                                  frameOptions.constructionExit))
      throw std::runtime_error(
          "--frame-state requires --frame-roundtrip --frame-heap --frame-outline without construction-prefix exits");
    if (frameOptions.stateExit && (!frameOptions.stateful || !frameDestroyIf.empty()))
      throw std::runtime_error(
          "--frame-state-exit requires --frame-state and excludes --frame-destroy-if");
    if (frameOptions.projectFields && (!frameOptions.stateful || frameStepBefore.empty()))
      throw std::runtime_error(
          "--frame-project-fields requires --frame-state and checkpoint boundaries");
    if ((!frameStepBefore.empty() && !frameOptions.stateful) ||
        (frameOptions.stateExit == isac::frames::RoundTripOptions::StateExit::Checkpoint &&
         frameStepBefore.empty()))
      throw std::runtime_error(
          "--frame-step-before requires --frame-state; checkpoint exit also requires a step boundary");
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
    if (library && mode == "native")
      throw std::runtime_error("--library requires --emit-obj/asm/llvm or inspection mode");
    if (input.empty()) {
      std::cerr << usage;
      return 1;
    }
    if ((nativeOptions.sanitize && mode != "native" && mode != "--emit-llvm") ||
        (nativeOptions.counters && mode != "native"))
      throw std::runtime_error("--sanitize requires native/LLVM emission; --runtime-test-counters "
                               "requires native emission");
    std::ifstream f(input);
    if (!f)
      throw std::runtime_error("cannot open input: " + input);
    std::stringstream b;
    b << f.rdbuf();
    PassTimer timer(timePasses);
    auto ts = isac::lex(b.str(), sources.add(input, b.str()));
    timer.mark("lexer");
    if (mode == "--emit-tokens") {
      for (auto &t : ts)
        std::cout << t.loc.line << ":" << t.loc.column << " " << t.text << "\n";
      return 0;
    }
    for (auto &t : ts)
      t.loc.file = input;
    auto compilation = isac::modules::compile(ts, &sources);
    timer.mark("imports/macros/parser");
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
    ast.library = library;
    if (mode == "--emit-ast") {
      isac::printAST(ast, std::cout);
      return 0;
    }
    auto h = isac::resolve(ast, nativeOptions.target);
    timer.mark("HIR");
    for (const auto &warning : h.warnings)
      isac::TerminalRenderer(sources, color).render(warning, std::cerr);
    if (mode == "--emit-hir") {
      isac::printHIR(h, std::cout);
      return 0;
    }
    auto core = isac::core::elaborate(std::move(h));
    timer.mark("Core elaboration");
    auto retain = mode == "--emit-captures" || mode == "--emit-frames";
    auto verified = isac::core::verify(std::move(core), {retain || frameRoundTrip});
    timer.mark("types/regions/resources");
    if (mode == "--emit-async-core" ||
        (mode == "--emit-core" && isac::asyncs::present(verified.get()))) {
      isac::core::print(verified.get(), std::cout);
      return 0;
    }
    if (isac::asyncs::present(verified.get()))
      verified = isac::asyncs::lower(verified, {retain || frameRoundTrip});
    timer.mark("async transformation/reverification");
    if (mode == "--emit-state-machine") {
      isac::core::print(verified.get(), std::cout);
      return 0;
    }
    auto namedFunction = [&](const std::string &name) {
      isac::FunctionId found = isac::None;
      for (const auto &f : verified.get().semantics.functions)
        if (f.name == name) {
          if (found != isac::None)
            throw std::runtime_error("ambiguous function: " + name);
          found = f.id;
        }
      if (found == isac::None)
        throw std::runtime_error("unknown function: " + name);
      return found;
    };
    if (!deferCall.empty()) {
      std::vector<isac::FunctionId> boundaries;
      if (!deferBefore.empty()) {
        std::istringstream names(deferBefore);
        for (std::string name; std::getline(names, name, ',');)
          boundaries.push_back(namedFunction(name));
        if (deferBefore.back() == ',')
          throw std::runtime_error("empty deferred boundary name");
      }
      verified = isac::frames::deferCalls(verified, namedFunction(deferCall), boundaries,
                                          {retain || frameRoundTrip});
      timer.mark("deferred segments/reverification");
    }
    if (!threadCall.empty()) {
      const auto worker = namedFunction(threadCall);
      verified = isac::execution::threadCalls(verified, worker, {retain || frameRoundTrip});
      nativeOptions.threads = true;
      timer.mark("thread contract/reverification");
    }
    if (frameRoundTrip) {
      verified = isac::frames::roundTripEntry(verified, frameOptions, {retain}, frameDestroyIf,
                                              frameStepBefore);
      timer.mark("frame storage/continuation/reverification");
    }
    if (mode == "--check")
      return 0;
    if (mode == "--emit-concurrency") {
      isac::concurrency::print(verified, std::cout);
      return 0;
    }
    if (mode == "--emit-captures") {
      isac::concurrency::printCaptures(verified, std::cout);
      return 0;
    }
    if (mode == "--emit-frames") {
      isac::frames::print(verified, std::cout);
      return 0;
    }
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
    nativeOptions.threads |= isac::execution::hasThreads(verified.get());
    auto erased = isac::erase(verified);
    timer.mark("erasure");
    if (mode == "--emit-c-bridge") {
      std::cout << erased.cBridge;
      return 0;
    }
    if (mode == "--emit-erased") {
      isac::runtime::print(erased, std::cout);
      return 0;
    }
    auto lir = isac::runtime::lower(erased);
    timer.mark("LIR");
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
    timer.mark("MLIR/LLVM/C bridge");
    if (stage == isac::BackendStage::LLVMIR)
      text = isac::optimizeLLVM(text, nativeOptions.target, options.optimization);
    timer.mark("LLVM optimization");
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
    timer.mark("emission/native link");
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
