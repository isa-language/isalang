#include "isac/diagnostics.hpp"
#include <algorithm>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif
namespace isac {
FileId SourceManager::add(std::string path, std::string text) {
  SourceFile f{std::move(path), std::move(text), {0}};
  for (size_t i = 0; i < f.text.size(); ++i)
    if (f.text[i] == '\n')
      f.lines.push_back(i + 1);
  files.push_back(std::move(f));
  return files.size() - 1;
}
const SourceFile &SourceManager::get(FileId id) const {
  return files.at(id);
}
Loc SourceManager::location(SourceSpan s) const {
  auto &f = get(s.file);
  auto off = std::min<size_t>(s.start, f.text.size());
  auto it = std::upper_bound(f.lines.begin(), f.lines.end(), off);
  unsigned line = it - f.lines.begin();
  return {line, unsigned(off - f.lines[line - 1] + 1), f.path, s};
}
const char *diagnosticCode(DiagnosticCode c) {
  switch (c) {
#define ISAC_DIAGNOSTIC(name, code)                                                                \
  case DiagnosticCode::name:                                                                       \
    return code;
#include "isac/diagnostic_codes.def"
#undef ISAC_DIAGNOSTIC
  }
  return "E9000";
}
Error::Error(Diagnostic d)
    : std::runtime_error(d.summary), loc(d.labels.empty() ? Loc{} : d.labels[0].origin),
      code(diagnosticCode(d.code)), diagnostic(std::move(d)) {}
DiagnosticBuilder::DiagnosticBuilder(DiagnosticCode code, Loc loc, std::string summary) {
  d.code = code;
  auto newline = summary.find('\n');
  d.summary = summary.substr(0, newline);
  if (newline != std::string::npos)
    d.notes.push_back(summary.substr(newline + 1));
  d.labels.push_back({loc, "", true});
  for (const auto &origin : loc.expansions) {
    Loc definition;
    definition.span = origin.definition;
    d.labels.push_back({definition, "macro `" + origin.name + "` defined here", false});
    d.notes.push_back("in expansion of @" + origin.name + " (#" + std::to_string(origin.id) + ")");
  }
}
DiagnosticBuilder &DiagnosticBuilder::label(Loc loc, std::string message) {
  d.labels[0] = {loc, std::move(message), true};
  return *this;
}
DiagnosticBuilder &DiagnosticBuilder::secondary(Loc loc, std::string message) {
  d.labels.push_back({loc, std::move(message), false});
  for (const auto &origin : loc.expansions) {
    auto note = "in expansion of @" + origin.name + " (#" + std::to_string(origin.id) + ")";
    if (std::find(d.notes.begin(), d.notes.end(), note) != d.notes.end())
      continue;
    Loc definition;
    definition.span = origin.definition;
    d.labels.push_back({definition, "macro `" + origin.name + "` defined here", false});
    d.notes.push_back(note);
  }
  return *this;
}
DiagnosticBuilder &DiagnosticBuilder::note(std::string s) {
  d.notes.push_back(std::move(s));
  return *this;
}
DiagnosticBuilder &DiagnosticBuilder::help(std::string s) {
  d.helps.push_back(std::move(s));
  return *this;
}
[[noreturn]] void DiagnosticBuilder::emit() const {
  throw Error(d);
}
void DiagnosticEngine::throwIfErrors() const {
  if (errors.empty())
    return;
  Error error(errors[0]);
  error.additional.assign(errors.begin() + 1, errors.end());
  if (errors.size() >= 20)
    error.additional.back().notes.push_back("aborting due to error limit (20)");
  throw error;
}
DiagnosticBuilder DiagnosticEngine::error(DiagnosticCode c, Loc l, std::string s) const {
  return {c, l, std::move(s)};
}
namespace {
std::pair<std::string, size_t> display(const std::string &s) {
  std::string out;
  size_t col = 0;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = s[i];
    if (c == '\t') {
      size_t n = 4 - col % 4;
      out.append(n, ' ');
      col += n;
      ++i;
    } else if (c < 128) {
      if (c >= 32)
        out += char(c);
      else
        out += '?';
      ++col;
      ++i;
    } else {
      size_t n = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : 2;
      if (i + n <= s.size() && validUtf8(s.substr(i, n))) {
        out += s.substr(i, n);
        ++col;
        i += n;
      } else {
        const char *hex = "0123456789abcdef";
        out += "\\x";
        out += hex[c >> 4];
        out += hex[c & 15];
        col += 4;
        ++i;
      }
    }
  }
  return {out, col};
}

}
void TerminalRenderer::render(const Diagnostic &d, std::ostream &o) const {
  const char *severity = d.severity == DiagnosticSeverity::Error     ? "error"
                         : d.severity == DiagnosticSeverity::Warning ? "warning"
                         : d.severity == DiagnosticSeverity::Note    ? "note"
                                                                     : "help";
#ifdef _WIN32
  DWORD mode = 0;
  bool terminal = GetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), &mode) &&
                  (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#else
  bool terminal = isatty(STDERR_FILENO);
#endif
  bool colored = color == ColorMode::Always || (color == ColorMode::Auto && terminal);
  if (colored)
    o << "\033[1;31m";
  o << severity << "[" << diagnosticCode(d.code) << "]";
  if (colored)
    o << "\033[0m";
  o << ": " << d.summary << "\n";
  std::map<FileId, std::vector<DiagnosticLabel>> groups;
  for (auto &label : d.labels)
    if (label.origin.span.file != NoFile)
      groups[label.origin.span.file].push_back(label);
  for (auto &[id, labels] : groups) {
    auto &f = sources.get(id);
    auto anchor = std::find_if(labels.begin(), labels.end(), [](auto &l) { return l.primary; });
    if (anchor == labels.end())
      anchor = labels.begin();
    auto loc = sources.location(anchor->origin.span);
    auto col = display(f.text.substr(f.lines[loc.line - 1],
                                     anchor->origin.span.start - f.lines[loc.line - 1]))
                   .second +
               1;
    o << "  --> " << f.path << ":" << loc.line << ":" << col << "\n   |\n";
    std::sort(labels.begin(), labels.end(),
              [](auto &a, auto &b) { return a.origin.span.start < b.origin.span.start; });
    std::set<unsigned> printed;
    for (auto &label : labels) {
      auto span = label.origin.span;
      auto first = sources.location(span).line;
      auto end = std::min<size_t>(std::max(span.start + 1, span.end), f.text.size());
      auto last = sources.location({id, uint32_t(end ? end - 1 : 0), uint32_t(end)}).line;
      for (unsigned line = first; line <= last; ++line) {
        auto begin = f.lines[line - 1];
        auto next = line < f.lines.size() ? f.lines[line] - 1 : f.text.size();
        auto start = std::clamp<size_t>(span.start, begin, next);
        auto finish = std::clamp<size_t>(end, begin, next);
        auto prefix = display(f.text.substr(begin, start - begin)).second;
        auto length = display(f.text.substr(begin, finish - begin)).second - prefix;
        if (printed.insert(line).second)
          o << std::setw(3) << line << " | " << display(f.text.substr(begin, next - begin)).first
            << "\n";
        o << "    | " << std::string(prefix, ' ')
          << std::string(std::max<size_t>(length, 1), label.primary ? '^' : '-');
        if (line == last && !label.message.empty())
          o << " " << label.message;
        o << "\n";
      }
    }
  }
  for (auto &note : d.notes) {
    std::istringstream lines(note);
    std::string line;
    bool first = true;
    while (std::getline(lines, line)) {
      o << (first ? "    = note: " : "            ") << line << "\n";
      first = false;
    }
  }
  for (auto &help : d.helps)
    o << "    = help: " << help << "\n";
}
std::string quoteText(const std::string &s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    switch (c) {
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    case 0:
      out += "\\0";
      break;
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    default:
      out += char(c);
    }
  }
  return out + "\"";
}
bool validUtf8(const std::string &s) {
  for (size_t i = 0; i < s.size();) {
    unsigned char c = s[i++];
    if (c < 128)
      continue;
    unsigned n;
    uint32_t cp, min;
    if (c >= 0xc2 && c <= 0xdf) {
      n = 1;
      cp = c & 31;
      min = 0x80;
    } else if (c >= 0xe0 && c <= 0xef) {
      n = 2;
      cp = c & 15;
      min = 0x800;
    } else if (c >= 0xf0 && c <= 0xf4) {
      n = 3;
      cp = c & 7;
      min = 0x10000;
    } else
      return false;
    if (i + n > s.size())
      return false;
    while (n--) {
      unsigned char b = s[i++];
      if ((b & 0xc0) != 0x80)
        return false;
      cp = (cp << 6) | (b & 63);
    }
    if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
      return false;
  }
  return true;
}
}
