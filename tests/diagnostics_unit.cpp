#include "isac/diagnostics.hpp"
#include <iostream>
#include <sstream>
int main() {
  using namespace isac;
  SourceManager sources;
  auto file = sources.add("source.isa", "\t\"Olá\" name\nnext\n");
  auto loc = sources.location({file, 8, 12});
  if (loc.line != 1 || loc.column != 9)
    return 1;
  Diagnostic d;
  d.severity = DiagnosticSeverity::Warning;
  d.code = DiagnosticCode::UnusedVariable;
  d.summary = "unused variable";
  d.labels.push_back({loc, "declared here", true});
  d.notes = {"test note"};
  d.helps = {"test help"};
  std::ostringstream out;
  TerminalRenderer(sources).render(d, out);
  if (out.str().find("warning[W1001]") == std::string::npos ||
      out.str().find("note: test note") == std::string::npos ||
      out.str().find("\033") != std::string::npos)
    return 2;
  if (!validUtf8("á😀") || validUtf8(std::string("\xc0\x80", 2)) ||
      validUtf8(std::string("\xed\xa0\x80", 3)))
    return 3;
  return 0;
}
