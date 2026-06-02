#include "compact/SessionMemoryCompact.h"

#include "memory/SessionMemory.h"

#include <cstdlib>
#include <algorithm>
#include <sstream>

namespace agent {
namespace compact {

SessionMemoryCompactConfig GetSessionMemoryCompactConfig() {
  SessionMemoryCompactConfig config;

  const char* envMin = std::getenv("AGENT_SM_COMPACT_MIN_TOKENS");
  if (envMin) {
    int val = std::atoi(envMin);
    if (val > 0) config.minTokens = val;
  }

  const char* envMax = std::getenv("AGENT_SM_COMPACT_MAX_TOKENS");
  if (envMax) {
    int val = std::atoi(envMax);
    if (val > 0) config.maxTokens = val;
  }

  const char* envMsgs = std::getenv("AGENT_SM_COMPACT_MIN_MSGS");
  if (envMsgs) {
    int val = std::atoi(envMsgs);
    if (val > 0) config.minTextBlockMessages = val;
  }

  return config;
}

bool ShouldUseSessionMemoryCompaction(bool enabled, bool sessionMemoryEmpty) {
  if (!enabled) return false;
  if (sessionMemoryEmpty) return false;

  // Check env override
  const char* envForce = std::getenv("AGENT_SM_COMPACT_FORCE");
  if (envForce && std::atoi(envForce) != 0) return true;

  return true;  // default: use if enabled and memory is non-empty
}

std::string BuildSessionMemoryContextInjection(
    const memory::SessionMemory* sessionMemory,
    int maxChars) {
  if (!sessionMemory) return std::string();

  // Get session-scoped memories
  auto entries = sessionMemory->ListMemories("", "session");

  // Also include project-scoped high-priority memories
  auto projectEntries = sessionMemory->ListMemories("", "project");

  // Merge and sort by priority (highest first)
  std::vector<memory::SessionMemoryEntry> allEntries;
  for (auto& e : entries) allEntries.push_back(e);
  for (auto& e : projectEntries) {
    // Only include high-priority project memories
    if (e.priority >= 3) allEntries.push_back(e);
  }

  std::sort(allEntries.begin(), allEntries.end(),
            [](const auto& a, const auto& b) {
              return a.priority > b.priority;
            });

  // Build compact injection
  std::ostringstream out;
  out << "[Session Memory]\n";
  int charsUsed = 0;

  for (const auto& entry : allEntries) {
    if (!entry.active) continue;
    std::string line = "- [" + entry.type + "] " + entry.content + "\n";
    if (charsUsed + static_cast<int>(line.size()) > maxChars) break;
    out << line;
    charsUsed += static_cast<int>(line.size());
  }

  if (charsUsed == 0) return std::string();
  return out.str();
}

std::string TruncateSessionMemoryForCompact(
    const std::string& fullContent,
    int maxChars) {
  if (fullContent.empty()) return std::string();
  if (static_cast<int>(fullContent.size()) <= maxChars) return fullContent;

  // Split into lines and keep the most recent/relevant ones
  std::vector<std::string> lines;
  std::istringstream stream(fullContent);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty()) lines.push_back(line);
  }

  // Keep from the end (most recent) within budget
  std::ostringstream out;
  int used = 0;
  for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
    int lineLen = static_cast<int>(it->size()) + 1;  // +1 for newline
    if (used + lineLen > maxChars) break;
    used += lineLen;
  }

  // Reconstruct in forward order
  int keepCount = 0;
  int countFromEnd = 0;
  for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
    int lineLen = static_cast<int>(it->size()) + 1;
    if (countFromEnd + lineLen > maxChars) break;
    countFromEnd += lineLen;
    ++keepCount;
  }

  std::size_t start = lines.size() >= static_cast<std::size_t>(keepCount)
      ? lines.size() - keepCount : 0;
  for (std::size_t i = start; i < lines.size(); ++i) {
    out << lines[i] << "\n";
  }

  return out.str();
}

}  // namespace compact
}  // namespace agent
