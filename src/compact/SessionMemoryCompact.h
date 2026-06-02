#pragma once

#include <string>
#include <vector>

namespace agent {
namespace memory { class SessionMemory; }

namespace compact {

// P0-03: Session memory compact configuration (aligned with local-ace sessionMemoryCompact).
// Controls thresholds for when to inject session memory into the system prompt
// during auto-compact, and how much context to preserve.

struct SessionMemoryCompactConfig {
  int minTokens = 10000;           // Minimum tokens to preserve after compaction
  int minTextBlockMessages = 5;    // Minimum messages with text content to keep
  int maxTokens = 40000;           // Maximum tokens to preserve (hard cap)
};

// Get current config (with env var overrides)
SessionMemoryCompactConfig GetSessionMemoryCompactConfig();

// Check whether session memory compaction should be used.
// Returns true when the feature is enabled and session memory is non-empty.
bool ShouldUseSessionMemoryCompaction(bool enabled, bool sessionMemoryEmpty);

// Build a session memory context injection string for the system prompt.
// Extracts relevant memories from the SessionMemory store and formats them
// as a compact injection block.
std::string BuildSessionMemoryContextInjection(
    const memory::SessionMemory* sessionMemory,
    int maxChars = 2000);

// Truncate session memory content to fit within a token budget.
// Strips older/less-relevant entries first.
std::string TruncateSessionMemoryForCompact(
    const std::string& fullContent,
    int maxChars = 3000);

}  // namespace compact
}  // namespace agent
