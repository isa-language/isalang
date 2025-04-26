#pragma once
#include <cstdint>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>
namespace isac {
using FileId = uint32_t;
inline constexpr FileId NoFile = UINT32_MAX;
struct SourceSpan {
  FileId file = NoFile;
  uint32_t start = 0, end = 0;
};
using SyntaxContext = uint32_t;
using ExpansionId = uint32_t;
struct ExpansionOrigin {
  ExpansionId id;
  SourceSpan definition;
  std::string name;
};
struct Loc {
  unsigned line = 1, column = 1;
  std::string file;
  SourceSpan span;
  SyntaxContext context = 0;
  std::vector<ExpansionOrigin> expansions = {};
};
struct SourceFile {
  std::string path, text;
  std::vector<uint32_t> lines;
};
class SourceManager {
  std::vector<SourceFile> files;

public:
  FileId add(std::string path, std::string text);
  const SourceFile &get(FileId id) const;
  Loc location(SourceSpan span) const;
};
enum class DiagnosticSeverity { Error, Warning, Note, Help };
enum class DiagnosticCode {
#define ISAC_DIAGNOSTIC(name, code) name,
#include "isac/diagnostic_codes.def"
#undef ISAC_DIAGNOSTIC
};
const char *diagnosticCode(DiagnosticCode);
struct DiagnosticLabel {
  Loc origin;
  std::string message;
  bool primary = true;
};
struct Diagnostic {
  DiagnosticSeverity severity = DiagnosticSeverity::Error;
  DiagnosticCode code = DiagnosticCode::InternalError;
  std::string summary;
  std::vector<DiagnosticLabel> labels;
  std::vector<std::string> notes, helps;
};
struct Error : std::runtime_error {
  Loc loc;
  std::string code;
  Diagnostic diagnostic;
  std::vector<Diagnostic> additional;
  explicit Error(Diagnostic d);
};
class DiagnosticBuilder {
  Diagnostic d;

public:
  DiagnosticBuilder(DiagnosticCode code, Loc loc, std::string summary);
  DiagnosticBuilder &label(Loc, std::string);
  DiagnosticBuilder &secondary(Loc, std::string);
  DiagnosticBuilder &note(std::string);
  DiagnosticBuilder &help(std::string);
  Diagnostic build() const {
    return d;
  }
  [[noreturn]] void emit() const;
};
class DiagnosticEngine {
  std::vector<Diagnostic> errors;

public:
  void report(Diagnostic d) {
    errors.push_back(std::move(d));
  }
  size_t errorCount() const {
    return errors.size();
  }
  void throwIfErrors() const;
  DiagnosticBuilder error(DiagnosticCode code, Loc loc, std::string summary) const;
};
[[noreturn]] inline void fail(Loc loc, DiagnosticCode code, std::string text) {
  DiagnosticEngine().error(code, loc, std::move(text)).emit();
}
enum class ColorMode { Auto, Always, Never };
class TerminalRenderer {
  const SourceManager &sources;
  ColorMode color;

public:
  TerminalRenderer(const SourceManager &s, ColorMode c = ColorMode::Never) : sources(s), color(c) {}
  void render(const Diagnostic &, std::ostream &) const;
};
bool validUtf8(const std::string &);
std::string quoteText(const std::string &);
}
