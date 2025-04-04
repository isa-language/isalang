#include "isac/backend.hpp"
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#ifndef ISAC_CLANG
#define ISAC_CLANG "clang"
#endif
namespace isac {
void compileNative(const std::string &ir, const std::string &output, NativeOptions options) {
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
  if (options.sanitize || options.counters)
    arguments.insert(arguments.end(), {"-x", "c", ISAC_RUNTIME_SOURCE, "-I", ISAC_RUNTIME_INCLUDE});
  else
    arguments.insert(arguments.end(), {"-x", "none", ISAC_RUNTIME_LIBRARY});
  arguments.insert(arguments.end(), {"-o", target});
  if (options.sanitize) {
    arguments.push_back("-fsanitize=address,undefined");
    arguments.push_back("-fno-omit-frame-pointer");
    arguments.push_back("-fno-sanitize-recover=all");
  }
  if (options.counters)
    arguments.push_back("-DISAC_RT_TESTING");
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
    if (errno == EINTR)
      continue;
    throw std::runtime_error("cannot wait for clang");
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    throw std::runtime_error("native compilation with clang failed");
}
}
