#include "isac/backend.hpp"
#include "isac/platform.hpp"
#include <llvm/Config/llvm-config.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Program.h>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <vector>
#ifndef ISAC_CLANG
#define ISAC_CLANG "clang"
#endif
namespace isac {
namespace {
std::string bundled(std::string_view relative, std::string_view configured) {
#ifdef _WIN32
  auto candidate = platform::executableDirectory().parent_path() / platform::filePath(relative);
  std::error_code error;
  if (std::filesystem::exists(candidate, error))
    return platform::pathText(candidate);
#else
  (void)relative;
#endif
  return std::string(configured);
}
std::string clangPath() {
#ifdef _WIN32
  auto candidate = platform::executableDirectory() / "clang.exe";
  if (std::filesystem::is_regular_file(candidate))
    return platform::pathText(candidate);
  if (auto found = llvm::sys::findProgramByName("clang.exe"))
    return *found;
#endif
  return ISAC_CLANG;
}
void runClang(std::vector<std::string> arguments) {
  arguments.at(0) = clangPath();
  std::vector<llvm::StringRef> args;
  for (const auto &argument : arguments)
    args.push_back(argument);
  std::string error;
  bool failed = false;
  int status =
      llvm::sys::ExecuteAndWait(arguments[0], args, std::nullopt, {}, 0, 0, &error, &failed);
  if (failed || status != 0)
    throw std::runtime_error("C ABI/native compilation with clang failed" +
                             (error.empty() ? std::string() : ": " + error));
}
struct TemporaryDirectory {
  std::filesystem::path path;
  TemporaryDirectory() {
    auto prefix = platform::pathText(std::filesystem::temp_directory_path() / "isac-abi");
    llvm::SmallString<128> directory;
    if (auto error = llvm::sys::fs::createUniqueDirectory(prefix, directory))
      throw std::runtime_error("cannot create temporary directory: " + error.message());
    path = platform::filePath(directory.str().str());
  }
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};
}
std::string compileCBridge(const std::string &source, const TargetInfo &target, bool sanitize) {
  TemporaryDirectory temp;
  auto c = platform::pathText(temp.path / "bridge.c"),
       ll = platform::pathText(temp.path / "bridge.ll");
  {
    std::ofstream file(platform::filePath(c));
    file << source;
    if (!file)
      throw std::runtime_error("cannot write C ABI bridge");
  }
  std::vector<std::string> args{
      ISAC_CLANG,       "--target=" + target.triple, "-std=c11", "-O0", "-S", "-emit-llvm",
      "-ffreestanding", "-fno-strict-aliasing",      "-o",       ll,    c};
  if (sanitize)

    args.insert(args.end(),
                {"-fsanitize=undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"});
#if LLVM_VERSION_MAJOR < 22

  args.insert(args.end(), {"-Xclang", "-disable-lifetime-markers"});
#endif
  runClang(std::move(args));
  std::ifstream file(platform::filePath(ll));
  std::string result((std::istreambuf_iterator<char>(file)), {});
  if (!file)
    throw std::runtime_error("cannot read C ABI bridge");
#if LLVM_VERSION_MAJOR < 22

  constexpr std::string_view newerAttribute = " nocreateundeforpoison ";
  for (size_t line = 0; line < result.size();) {
    auto end = result.find('\n', line);
    if (end == std::string::npos)
      end = result.size();
    if (result.compare(line, 12, "attributes #") == 0) {
      auto attribute = result.find(newerAttribute, line);
      if (attribute < end) {
        result.erase(attribute, newerAttribute.size() - 1);
        end -= newerAttribute.size() - 1;
      }
    }
    line = end + 1;
  }
#endif
  return result;
}
void compileNative(const std::string &ir, const std::string &output, NativeOptions options) {
  if (!options.target.triple.empty() && options.target.triple != hostTriple())
    throw std::runtime_error(
        "cross-linking requires a target runtime, linker and sysroot; use --emit-obj");
  TemporaryDirectory cleanup;
  auto ll = platform::pathText(cleanup.path / "program.ll");
  {
    std::ofstream f(platform::filePath(ll));
    f << ir;
    if (!f)
      throw std::runtime_error("cannot write temporary LLVM IR");
  }
  auto outputPath = std::filesystem::absolute(platform::filePath(output));
#ifdef _WIN32
  if (outputPath.extension().empty())
    outputPath += ".exe";
#endif
  std::string target = platform::pathText(outputPath);
  std::vector<std::string> arguments = {ISAC_CLANG, "-Wno-override-module", "-x", "ir", ll};
  static const char *levels[] = {"-O0", "-O1", "-O2", "-O3", "-Os", "-Oz"};
  arguments.push_back(levels[static_cast<unsigned>(options.optimization)]);
  if (!options.target.triple.empty())
    arguments.push_back("--target=" + options.target.triple);
  if (options.sanitize || options.counters) {
    arguments.insert(arguments.end(),
                     {"-x", "c", bundled("runtime/src/isac_runtime.c", ISAC_RUNTIME_SOURCE)});
    if (std::string_view(ISAC_ALLOCATOR_SOURCE).size())
      arguments.push_back(bundled("runtime/src/isac_allocator_libc.c", ISAC_ALLOCATOR_SOURCE));
    if (options.threads) {
      auto threadSource = bundled("runtime/src/isac_threads.c", ISAC_THREAD_SOURCE);
      if (!std::filesystem::exists(platform::filePath(threadSource)))
        throw std::runtime_error("selected runtime lacks the scoped thread source ABI");
      arguments.push_back(threadSource);
    }
    arguments.insert(arguments.end(), {"-I", bundled("runtime/include", ISAC_RUNTIME_INCLUDE)});
    if (std::string_view(ISAC_ALLOCATOR_SOURCE).empty())

      arguments.insert(arguments.end(),
                       {"-x", "none", bundled("runtime/libisac_runtime.a", ISAC_RUNTIME_LIBRARY)});
  } else
    arguments.insert(arguments.end(),
                     {"-x", "none", bundled("runtime/libisac_runtime.a", ISAC_RUNTIME_LIBRARY)});
#ifdef _WIN32
  arguments.insert(arguments.end(), {"-lshell32", "-Wl,--stack,16777216"});
#else
  if (options.threads)
    arguments.push_back("-pthread");
#endif
  if (!options.linkInputs.empty()) {
    arguments.push_back("-x");
    arguments.push_back("none");
  }
  arguments.insert(arguments.end(), options.linkInputs.begin(), options.linkInputs.end());
  for (const auto &path : options.libraryPaths)
    arguments.push_back("-L" + path);
  for (const auto &lib : options.libraries)
    arguments.push_back("-l" + lib);
  arguments.insert(arguments.end(), {"-o", target});
  if (options.sanitize) {
    arguments.push_back("-fsanitize=address,undefined");
    arguments.push_back("-fno-omit-frame-pointer");
    arguments.push_back("-fno-sanitize-recover=all");
  }
  if (options.counters)
    arguments.push_back("-DISAC_RT_TESTING");
  runClang(std::move(arguments));
}
}
