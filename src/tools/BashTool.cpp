// BashTool.cpp
#include "tools/BashTool.h"
#include <sstream>
namespace agent { namespace tools {
const char* kBashToolName = "Bash";
const char* kBashToolDescription =
  "Executes a given command in a PowerShell shell.\n"
  "- The shell is stateful: environment variables, current directory, and other state persist across commands.\n"
  "- Commands execute in the workspace root by default.\n"
  "- Long-running commands can be configured with a timeout (default 120s).\n"
  "- Use PowerShell commands (Select-String, Get-ChildItem) instead of Unix equivalents (grep, ls).\n"
  "- For Python, use python or python -c for inline scripts.";
std::string GetBashToolInputSchema() {
  return "{"  
    "\"type\":\"object\","
    "\"properties\":{"
      "\"command\":{\"type\":\"string\",\"description\":\"The shell command to execute\"},"
      "\"description\":{\"type\":\"string\",\"description\":\"A short description of what this command does\"},"
      "\"timeout\":{\"type\":\"number\",\"description\":\"Optional timeout in milliseconds (default 120000)\"}"
    "},"
    "\"required\":[\"command\"]"
  "}";
}
std::string BuildBashToolPrompt() {
  std::ostringstream p; p << "- " << kBashToolName << ": " << kBashToolDescription; return p.str();
}
}}