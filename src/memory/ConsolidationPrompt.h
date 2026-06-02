// ConsolidationPrompt.h — aligned with local-ace services/autoDream/consolidationPrompt.ts
// Builds the /dream consolidation prompt for background memory consolidation.
#pragma once

#include <string>

namespace agent {
namespace memory {

// Build the consolidation prompt for the /dream background memory task.
// Mirrors local-ace buildConsolidationPrompt(memoryRoot, transcriptDir, extra).
//
// The prompt guides a forked subagent through 4 phases:
//   1. Orient — explore existing memory files
//   2. Gather — collect recent signal from daily logs and transcripts
//   3. Consolidate — write/update memory files
//   4. Prune and Index — update the entrypoint index
std::string BuildConsolidationPrompt(const std::string& memoryRoot,
                                     const std::string& transcriptDir,
                                     const std::string& extra = "");

}  // namespace memory
}  // namespace agent
