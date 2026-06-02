// FileWriteTool.cpp
#include "tools/FileWriteTool.h"
#include <sstream>
namespace agent { namespace tools {
const char* kFileWriteToolName = "Write";
const char* kFileWriteToolDescription =
  "Writes a file to the local filesystem.\n"
  "- This tool will overwrite the existing file if there is one.\n"
  "- If this is an existing file, you MUST use the Read tool first to read "
  "the file contents.\n"
  "- ALWAYS prefer editing existing files using FileEdit tool in the codebase. "
  "NEVER write new files unless explicitly required.";
std::string GetFileWriteToolInputSchema() {
  return "{\"type\":\"object\",\"properties\":{\"file_path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"file_path\",\"content\"]}";
}
std::string BuildFileWriteToolPrompt() {
  std::ostringstream p; p << "- " << kFileWriteToolName << ": " << kFileWriteToolDescription; return p.str();
}
}}