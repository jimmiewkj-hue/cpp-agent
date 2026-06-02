// FileReadTool.cpp
#include "tools/FileReadTool.h"
#include <sstream>
namespace agent { namespace tools {
const char* kFileReadToolName = "Read";
const char* kFileReadToolDescription =
  "Reads a file from the local filesystem. You can access any file directly.\n"
  "- Specify offset and limit for long files, but it is recommended to read "
  "the whole file by not providing these parameters.\n"
  "- You can call multiple tools in a single response. It is always better "
  "to speculatively read multiple files as a batch that are potentially useful.";
std::string GetFileReadToolInputSchema() {
  return "{\"type\":\"object\",\"properties\":{\"file_path\":{\"type\":\"string\"},\"offset\":{\"type\":\"integer\"},\"limit\":{\"type\":\"integer\"}},\"required\":[\"file_path\"]}";
}
std::string BuildFileReadToolPrompt() {
  std::ostringstream p; p << "- " << kFileReadToolName << ": " << kFileReadToolDescription; return p.str();
}
}}