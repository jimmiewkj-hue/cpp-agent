#include "tools/WebSearchTool.h"
#include <sstream>
namespace agent { namespace tools {
const char* kWebSearchToolName = "WebSearch";
const char* kWebSearchToolDescription =
  "Allows AI coding assistant to search the web and use the results to inform responses.\n"
  "- Provides up-to-date information for current events and recent data\n"
  "- Returns search result information formatted as search result blocks\n"
  "- Use this tool for accessing information beyond AI coding assistant's knowledge cutoff\n"
  "- Searches are performed automatically within a single API call\n\n"
  "Usage notes:\n"
  "- Domain filtering is supported to include or block specific websites\n"
  "- Web search is only available in the US and Canada\n"
  "- Domain filtering is only available in the US";
std::string GetWebSearchToolInputSchema() {
  return "{"
    "\"type\":\"object\","
    "\"properties\":{"
      "\"query\":{\"type\":\"string\",\"description\":\"The search query to look up on the web\"}"
    "},"
    "\"required\":[\"query\"]"
  "}";
}
std::string BuildWebSearchToolPrompt() {
  std::ostringstream p; p << "- " << kWebSearchToolName << ": " << kWebSearchToolDescription; return p.str();
}
}}