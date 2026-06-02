// GlobTool.cpp
#include "tools/GlobTool.h"
#include <sstream>
namespace agent { namespace tools {
const char* kGlobToolName = "Glob";
const char* kGlobToolDescription =
  "Find files matching a glob pattern.\n"
  "- Returns relative file paths.\n"
  "- Prefer this over Bash ls/find for file search.";
std::string GetGlobToolInputSchema() {
  return "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"}},\"required\":[\"pattern\"]}";
}
std::string BuildGlobToolPrompt() {
  std::ostringstream p; p << "- " << kGlobToolName << ": " << kGlobToolDescription; return p.str();
}
}}