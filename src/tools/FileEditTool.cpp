// FileEditTool.cpp
#include "tools/FileEditTool.h"
#include <sstream>
namespace agent { namespace tools {
const char* kFileEditToolName = "Edit";
const char* kFileEditToolDescription =
  "Performs string replacements in an existing file with multi-level fuzzy matching.\n"
  "- The tool first tries exact byte-level matching, then falls back through:\n"
  "  1. CRLF normalization (handles \\r\\n vs \\n differences)\n"
  "  2. Quote normalization (handles curly vs straight quotes) + CRLF\n"
  "  3. Trailing whitespace stripping + CRLF (skipped for .md/.mdx files)\n"
  "  4. Full normalization (all of the above combined)\n"
  "  5. Line-anchor fallback (uses first/last lines as anchors with 50%% interior match)\n"
  "- XML entity de-sanitization is applied automatically (&lt; &gt; &amp; etc.)\n"
  "- When curly quotes are detected in the file, the tool preserves the quote style in new_string.\n"
  "- When editing text, try to preserve the exact indentation (tabs/spaces) as it appears before.\n"
  "- ALWAYS prefer editing existing files in the codebase. NEVER write new files unless explicitly required.\n"
  "- If the edit fails, READ the file first to get current content, then retry with exact old_string.\n"
  "- If the edit fails, the error message will list all attempted matching strategies and possible causes.";
std::string GetFileEditToolInputSchema() {
  return "{\"type\":\"object\",\"properties\":{\"file_path\":{\"type\":\"string\"},\"old_string\":{\"type\":\"string\"},\"new_string\":{\"type\":\"string\"},\"replace_all\":{\"type\":\"boolean\",\"default\":false}},\"required\":[\"file_path\",\"old_string\",\"new_string\"]}";
}
std::string BuildFileEditToolPrompt() {
  std::ostringstream p; p << "- " << kFileEditToolName << ": " << kFileEditToolDescription; return p.str();
}
}}