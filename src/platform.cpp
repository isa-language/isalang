#include "isac/platform.hpp"
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace isac::platform {
std::filesystem::path filePath(std::string_view text) {
  return std::filesystem::path(
      std::u8string(reinterpret_cast<const char8_t *>(text.data()), text.size()));
}
std::string pathText(const std::filesystem::path &path) {
  auto text = path.generic_u8string();
  return {reinterpret_cast<const char *>(text.data()), text.size()};
}
std::filesystem::path executableDirectory() {
#ifdef _WIN32
  std::vector<wchar_t> buffer(512);
  for (;;) {
    auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!length)
      throw std::runtime_error("cannot locate the running compiler");
    if (length < buffer.size())
      return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
    if (buffer.size() >= 32768)
      throw std::runtime_error("compiler executable path is too long");
    buffer.resize(buffer.size() * 2);
  }
#else
  return {};
#endif
}
std::string libraryRoot(std::string_view configured) {
#ifdef _WIN32
  auto bundled = executableDirectory().parent_path() / "lib";
  std::error_code error;
  if (std::filesystem::is_regular_file(bundled / "std" / "prelude.isa", error))
    return pathText(bundled);
#endif
  return std::string(configured);
}
DriverArguments::DriverArguments(int argc, char **argv) : count(argc), values(argv) {
#ifdef _WIN32
  int wideCount = 0;
  auto wide = CommandLineToArgvW(GetCommandLineW(), &wideCount);
  if (!wide)
    throw std::runtime_error("cannot decode Windows command-line arguments");
  struct Release {
    wchar_t **value;
    ~Release() {
      LocalFree(value);
    }
  } release{wide};
  storage.reserve(wideCount);
  pointers.reserve(wideCount + 1);
  for (int n = 0; n < wideCount; ++n) {
    auto bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[n], -1, nullptr, 0,
                                     nullptr, nullptr);
    if (!bytes)
      throw std::runtime_error("Windows command-line argument is not valid Unicode");
    std::string text(static_cast<size_t>(bytes), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[n], -1, text.data(), bytes,
                             nullptr, nullptr))
      throw std::runtime_error("cannot encode Windows command-line argument as UTF-8");
    text.pop_back();
    storage.push_back(std::move(text));
  }
  for (auto &text : storage)
    pointers.push_back(text.data());
  pointers.push_back(nullptr);
  count = wideCount;
  values = pointers.data();
#endif
}
}
