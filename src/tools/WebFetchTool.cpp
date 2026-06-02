#include "tools/WebFetchTool.h"
#include <sstream>
namespace agent { namespace tools {
const char* kWebFetchToolName = "WebFetch";
const char* kWebFetchToolDescription =
  "Fetches content from a URL and processes it with a prompt.\n"
  "- Fetches content from a specified URL and processes using an AI model\n"
  "- Takes a URL and a prompt as input\n"
  "- Fetches the URL content, converts HTML to markdown\n"
  "- Processes the content with the prompt using a lightweight model\n"
  "- Returns the model's response about the content\n"
  "- Use this tool when you need to retrieve and analyze web content\n\n"
  "Usage notes:\n"
  "- IMPORTANT: If an MCP-provided web fetch tool is available, prefer using that tool\n"
  "  instead of this one, as it may have fewer restrictions.\n"
  "- The URL must be a fully-formed valid URL\n"
  "- HTTP URLs will be automatically upgraded to HTTPS\n"
  "- The prompt should describe what information you want to extract from the page\n"
  "- Results may be summarized if the content is very large\n"
  "- When a URL redirects to a different host, the tool will inform you and provide\n"
  "  the redirect URL in a special format. You should then make a new WebFetch request\n"
  "  with the redirect URL to fetch the content.";
std::string GetWebFetchToolInputSchema() {
  return "{"
    "\"type\":\"object\","
    "\"properties\":{"
      "\"url\":{\"type\":\"string\",\"description\":\"The URL to fetch content from\"},"
      "\"prompt\":{\"type\":\"string\",\"description\":\"The prompt to run on the fetched content\"}"
    "},"
    "\"required\":[\"url\",\"prompt\"]"
  "}";
}
std::string BuildWebFetchToolPrompt() {
  std::ostringstream p; p << "- " << kWebFetchToolName << ": " << kWebFetchToolDescription; return p.str();
}
}}