#include "tools/ToolSearch.h"

#include "tools/ToolRegistry.h"
#include "tools/Tool.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace agent {
namespace tools {

namespace {

std::string ToLower(const std::string& s) {
  std::string result = s;
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return result;
}

bool ContainsWord(const std::string& haystack, const std::string& needle) {
  return ToLower(haystack).find(ToLower(needle)) != std::string::npos;
}

}  // namespace

int ComputeToolRelevance(const std::string& toolName,
                          const std::string& description,
                          const std::string& query) {
  std::string nameLower = ToLower(toolName);
  std::string descLower = ToLower(description);
  std::string queryLower = ToLower(query);

  int score = 0;

  // Exact name match = highest score
  if (nameLower == queryLower) {
    score += 100;
  }

  // Name prefix match
  if (nameLower.find(queryLower) == 0) {
    score += 50;
  }

  // Name contains query
  if (nameLower.find(queryLower) != std::string::npos) {
    score += 30;
  }

  // Split query into words and check each
  std::istringstream qs(queryLower);
  std::string word;
  while (qs >> word) {
    if (word.size() < 2) continue;  // Skip single-char words
    if (nameLower.find(word) != std::string::npos) {
      score += 10;
    }
    if (descLower.find(word) != std::string::npos) {
      score += 5;
    }
  }

  // Description contains full query
  if (descLower.find(queryLower) != std::string::npos) {
    score += 15;
  }

  return score;
}

ToolSearchResult SearchTools(const ToolRegistry* registry,
                              const std::string& query,
                              int maxResults) {
  ToolSearchResult result;
  result.query = query;

  if (!registry) return result;

  // Handle "select:<name>" pattern
  if (query.size() > 7 && ToLower(query.substr(0, 7)) == "select:") {
    std::string targetName = query.substr(7);
    // Trim whitespace
    while (!targetName.empty() && targetName.front() == ' ') {
      targetName.erase(0, 1);
    }

    const auto tools = registry->ListTools();
    for (const auto& tool : tools) {
      if (ToLower(tool->Name()) == ToLower(targetName)) {
        ToolSearchMatch match;
        match.toolName = tool->Name();
        match.description = tool->UserFacingDescription();
        match.relevanceScore = 100;
        result.matches.push_back(match);
        return result;
      }
    }
  }

  // Keyword search
  const auto tools = registry->ListTools();
  result.totalTools = static_cast<int>(tools.size());

  std::vector<std::pair<int, ToolSearchMatch>> scored;
  for (const auto& tool : tools) {
    int score = ComputeToolRelevance(
        tool->Name(), tool->UserFacingDescription(), query);

    if (score > 0) {
      ToolSearchMatch match;
      match.toolName = tool->Name();
      match.description = tool->UserFacingDescription();
      match.relevanceScore = score;
      scored.push_back({score, match});
    }
  }

  // Sort by relevance (highest first)
  std::sort(scored.begin(), scored.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });

  for (int i = 0; i < std::min(maxResults, static_cast<int>(scored.size())); ++i) {
    result.matches.push_back(scored[i].second);
  }

  return result;
}

std::string FormatToolSearchResults(const ToolSearchResult& results) {
  std::ostringstream out;

  if (results.matches.empty()) {
    out << "No tools found matching query: " << results.query << "\n";
    out << "Total tools available: " << results.totalTools << "\n";
    return out.str();
  }

  out << "Found " << results.matches.size() << " tool(s) matching \""
      << results.query << "\":\n\n";

  for (const auto& match : results.matches) {
    out << "- **" << match.toolName << "**";
    if (match.isDeferred) out << " [deferred]";
    out << "\n";
    if (!match.description.empty()) {
      // Limit description to first 120 chars
      std::string desc = match.description;
      if (desc.size() > 120) desc = desc.substr(0, 117) + "...";
      out << "  " << desc << "\n";
    }
  }

  if (results.totalDeferredTools > 0) {
    out << "\n" << results.totalDeferredTools << " deferred tools available. "
        << "Use ToolSearch with 'select:<name>' to activate.\n";
  }

  return out.str();
}

bool SelectDeferredTool(const std::string& toolName) {
  (void)toolName;
  // In a full implementation, this would register/unhide a deferred tool
  return false;
}

}  // namespace tools
}  // namespace agent
