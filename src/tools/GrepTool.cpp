// GrepTool.cpp
#include "tools/GrepTool.h"
#include <sstream>
namespace agent { namespace tools {
const char* kGrepToolName = "Grep";
const char* kGrepToolDescription =
  "Search for a pattern in files.\n"
  "- Uses ripgrep for fast searching.\n"
  "- Supports full regex syntax.\n"
  "- Prefer grep for exact symbol/string searches.";
std::string GetGrepToolInputSchema() {
  return "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"include\":{\"type\":\"string\"}},\"required\":[\"pattern\"]}";
}
std::string BuildGrepToolPrompt() {
  std::ostringstream p; p << "- " << kGrepToolName << ": " << kGrepToolDescription; return p.str();
}
}}