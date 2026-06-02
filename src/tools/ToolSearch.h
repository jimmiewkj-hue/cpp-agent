#pragma once

#include <string>
#include <vector>

namespace agent {
namespace tools {

// P0-03: Tool search result (aligned with local-ace ToolSearchTool).
// Allows the agent to search across all registered tools by keyword or description.

struct ToolSearchMatch {
  std::string toolName;
  std::string description;
  int relevanceScore = 0;   // Higher = better match
  bool isDeferred = false;  // Tool requires explicit activation
};

struct ToolSearchResult {
  std::vector<ToolSearchMatch> matches;
  std::string query;
  int totalTools = 0;
  int totalDeferredTools = 0;
};

class ToolRegistry;  // forward decl

// Search for tools matching the given query.
// Query can be:
//   "select:<tool_name>" ? direct selection
//   keywords ? fuzzy match against name and description
ToolSearchResult SearchTools(const ToolRegistry* registry,
                              const std::string& query,
                              int maxResults = 5);

// Format search results for display to the agent.
std::string FormatToolSearchResults(const ToolSearchResult& results);

// Select a deferred tool by name. Returns true if tool was found and activated.
bool SelectDeferredTool(const std::string& toolName);

// Check if a tool name matches a search query (case-insensitive, fuzzy).
int ComputeToolRelevance(const std::string& toolName,
                          const std::string& description,
                          const std::string& query);

}  // namespace tools
}  // namespace agent
