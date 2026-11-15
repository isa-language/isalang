#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace isac::platform {

std::filesystem::path filePath(std::string_view);
std::string pathText(const std::filesystem::path &);
std::filesystem::path executableDirectory();
std::string libraryRoot(std::string_view configured);
struct DriverArguments {
  int count;
  char **values;
  DriverArguments(int argc, char **argv);
  DriverArguments(const DriverArguments &) = delete;
  DriverArguments &operator=(const DriverArguments &) = delete;

private:
  std::vector<std::string> storage;
  std::vector<char *> pointers;
};
}
