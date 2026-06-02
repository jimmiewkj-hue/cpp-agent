// CompactPrompt.h — aligned with local-ace services/compact/prompt.ts (374 lines)
// Provides the BASE_COMPACT_PROMPT, PARTIAL_COMPACT_PROMPT, NO_TOOLS_PREAMBLE,
// and builder functions for constructing compact summary prompts.
#pragma once

#include <string>

namespace agent {
namespace compact {

// Direction for partial compact: "from" (summary precedes recent) or "up_to" (summary followed by newer)
enum class PartialCompactDirection { From, UpTo };

// ============================================================================
// Prompt Constants — mirrors local-ace prompt.ts
// ============================================================================

// Aggressive no-tools preamble for cache-sharing fork paths
// Mirrors local-ace NO_TOOLS_PREAMBLE
extern const char* kNoToolsPreamble;

// Detailed analysis instruction for full conversation scope
// Mirrors local-ace DETAILED_ANALYSIS_INSTRUCTION_BASE
extern const char* kDetailedAnalysisInstructionBase;

// Detailed analysis instruction for recent messages scope
// Mirrors local-ace DETAILED_ANALYSIS_INSTRUCTION_PARTIAL
extern const char* kDetailedAnalysisInstructionPartial;

// Base compact prompt with 9 sections (Primary Request, Technical Concepts,
// Files & Code, Errors & Fixes, Problem Solving, User Messages, Pending Tasks,
// Current Work, Next Step)
// Mirrors local-ace BASE_COMPACT_PROMPT
extern const char* kBaseCompactPrompt;

// Partial compact prompt ("from" direction: summary precedes kept recent messages)
// Mirrors local-ace PARTIAL_COMPACT_PROMPT
extern const char* kPartialCompactPrompt;

// Partial compact prompt ("up_to" direction: summary will be followed by newer messages)
// Mirrors local-ace PARTIAL_COMPACT_UP_TO_PROMPT
extern const char* kPartialCompactUpToPrompt;

// Trailer appended at end of all compact prompts
// Mirrors local-ace NO_TOOLS_TRAILER
extern const char* kNoToolsTrailer;

// ============================================================================
// Builder Functions — mirrors local-ace getCompactPrompt / getPartialCompactPrompt
// ============================================================================

// Build the full base compact prompt
// Mirrors local-ace getCompactPrompt(customInstructions)
std::string BuildCompactPrompt(const std::string& customInstructions = "");

// Build a partial compact prompt
// Mirrors local-ace getPartialCompactPrompt(customInstructions, direction)
std::string BuildPartialCompactPrompt(
    const std::string& customInstructions = "",
    PartialCompactDirection direction = PartialCompactDirection::From);

// Build the no-tools preamble only (for cache-sharing fork paths)
// Mirrors local-ace NO_TOOLS_PREAMBLE concatenation
std::string BuildNoToolsPreamble();

}  // namespace compact
}  // namespace agent
