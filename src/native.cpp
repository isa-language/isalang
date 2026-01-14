#include "isac/backend.hpp"
#include <llvm/Config/llvm-config.h>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#ifndef ISAC_CLANG
#define ISAC_CLANG "clang"
#endif
namespace isac {
namespace {
void runClang(std::vector<std::string> arguments) {
  std::vector<char *> args;
  for (auto &argument : arguments)
    args.push_back(argument.data());
  args.push_back(nullptr);
  pid_t pid = fork();
  if (pid < 0)
    throw std::runtime_error("cannot spawn clang");
  if (pid == 0) {
    execv(ISAC_CLANG, args.data());
    _exit(127);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR)
      throw std::runtime_error("cannot wait for clang");
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    throw std::runtime_error("C ABI/native compilation with clang failed");
}
struct TemporaryDirectory {
  std::filesystem::path path;
  TemporaryDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() / "isac-abi-XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back(0);
    auto *dir = mkdtemp(name.data());
    if (!dir)
      throw std::runtime_error("cannot create C ABI temporary directory");
    path = dir;
  }
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};
}
std::string compileCBridge(const std::string &source, const TargetInfo &target, bool sanitize) {
  TemporaryDirectory temp;
  auto c = (temp.path / "bridge.c").string(), ll = (temp.path / "bridge.ll").string();
  {
    std::ofstream file(c);
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
  std::ifstream file(ll);
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
  std::string pattern = (std::filesystem::temp_directory_path() / "isac-XXXXXX").string();
  std::vector<char> tmp(pattern.begin(), pattern.end());
  tmp.push_back(0);
  char *dir = mkdtemp(tmp.data());
  if (!dir)
    throw std::runtime_error("cannot create temporary directory");
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
  } cleanup{dir};
  auto ll = (cleanup.path / "program.ll").string();
  {
    std::ofstream f(ll);
    f << ir;
    if (!f)
      throw std::runtime_error("cannot write temporary LLVM IR");
  }
  std::string target = std::filesystem::absolute(output).string();
  std::vector<std::string> arguments = {ISAC_CLANG, "-Wno-override-module", "-x", "ir", ll};
  static const char *levels[] = {"-O0", "-O1", "-O2", "-O3", "-Os", "-Oz"};
  arguments.push_back(levels[static_cast<unsigned>(options.optimization)]);
  if (!options.target.triple.empty())
    arguments.push_back("--target=" + options.target.triple);
  if (options.sanitize || options.counters)
    arguments.insert(arguments.end(), {"-x", "c", ISAC_RUNTIME_SOURCE, ISAC_ALLOCATOR_SOURCE, "-I",
                                       ISAC_RUNTIME_INCLUDE});
  else
    arguments.insert(arguments.end(), {"-x", "none", ISAC_RUNTIME_LIBRARY});
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
