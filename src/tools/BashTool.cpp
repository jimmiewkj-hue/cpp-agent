// BashTool.cpp
#include "tools/BashTool.h"
#include <sstream>
namespace agent { namespace tools {
const char* kBashToolName = "Bash";
const char* kBashToolDescription =
  "Executes a command in a PowerShell (Windows) shell.\n"
  "- The shell is stateful: environment variables, current directory persist.\n"
  "- Commands execute in the workspace root by default.\n"
  "- Long-running commands timeout after 120 seconds.\n"
  "- AVOID Unix commands (grep, head, tail) - use PowerShell equivalents (Select-String, Select-Object).\n"
  "- For Python, use python or python -c for inline scripts.";
std::string GetBashToolInputSchema() {
  return "{"
    "\"type\":\"object\","
    "\"properties\":{"
      "\"command\":{\"type\":\"string\"},"
      "\"description\":{\"type\":\"string\"},"
      "\"timeout\":{\"type\":\"number\"}"
    "},"
    "\"required\":[\"command\"]"
  "}";
}
std::string BuildBashToolPrompt() {
  std::ostringstream p; p << "- " << kBashToolName << ": " << kBashToolDescription; return p.str();
}
}}