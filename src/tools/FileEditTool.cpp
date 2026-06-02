// FileEditTool.cpp
#include "tools/FileEditTool.h"
#include <sstream>
namespace agent { namespace tools {
const char* kFileEditToolName = "Edit";
const char* kFileEditToolDescription =
  "Performs exact string replacements in an existing file.\n"
  "- When editing text, ensure you preserve the exact indentation (tabs/spaces) as it appears before.\n"
  "- ALWAYS prefer editing existing files in the codebase. NEVER write new files unless explicitly required.";
std::string GetFileEditToolInputSchema() {
  return "{\"type\":\"object\",\"properties\":{\"file_path\":{\"type\":\"string\"},\"old_string\":{\"type\":\"string\"},\"new_string\":{\"type\":\"string\"}},\"required\":[\"file_path\",\"old_string\",\"new_string\"]}";
}
std::string BuildFileEditToolPrompt() {
  std::ostringstream p; p << "- " << kFileEditToolName << ": " << kFileEditToolDescription; return p.str();
}
}}