// PostCompactCleanup.h — aligned with local-ace services/compact/postCompactCleanup.ts
// Runs cleanup of caches and tracking state after compaction.
// Call after both auto-compact and manual /compact to free memory
// held by tracking structures that are invalidated by compaction.
#pragma once

namespace agent {
namespace compact {

// Run cleanup after compaction completes.
// Resets microcompact state, clears caches, and frees memory.
// Mirrors local-ace runPostCompactCleanup.
//
// isMainThread: true for main-thread compacts, false for subagent compacts.
// Subagents should skip main-thread module-level state resets.
void RunPostCompactCleanup(bool isMainThread = true);


// Query whether post-compact cleanup has been executed at least once.
// Useful for testing and for callers that want to skip redundant cleanup.
bool IsPostCompactCleanupDone();

// Return the total number of compactions processed (across all threads).
int GetCompactCount();
}  // namespace compact
}  // namespace agent
