// ConsolidationPrompt.cpp — aligned with local-ace services/autoDream/consolidationPrompt.ts
#include "memory/ConsolidationPrompt.h"

#include <sstream>

namespace agent {
namespace memory {

// Constants from memdir alignment (mirrors local-ace memdir constants)
static const char* kEntrypointName = "MEMORY.md";
static const int kMaxEntrypointLines = 150;
static const char* kDirExistsGuidance =
    "(create it if it doesn't exist — use mkdir)";

std::string BuildConsolidationPrompt(const std::string& memoryRoot,
                                     const std::string& transcriptDir,
                                     const std::string& extra) {
  std::ostringstream prompt;

  prompt << "# Dream: Memory Consolidation\n\n";
  prompt << "You are performing a dream — a reflective pass over your memory "
            "files. Synthesize what you've learned recently into durable, "
            "well-organized memories so that future sessions can orient quickly.\n\n";
  prompt << "Memory directory: `" << memoryRoot << "`\n";
  prompt << kDirExistsGuidance << "\n\n";
  prompt << "Session transcripts: `" << transcriptDir
         << "` (large JSONL files — grep narrowly, don't read whole files)\n\n";
  prompt << "---\n\n";
  prompt << "## Phase 1 — Orient\n\n";
  prompt << "- `ls` the memory directory to see what already exists\n";
  prompt << "- Read `" << kEntrypointName
         << "` to understand the current index\n";
  prompt << "- Skim existing topic files so you improve them rather than "
            "creating duplicates\n";
  prompt << "- If `logs/` or `sessions/` subdirectories exist (assistant-mode "
            "layout), review recent entries there\n\n";
  prompt << "## Phase 2 — Gather recent signal\n\n";
  prompt << "Look for new information worth persisting. Sources in rough "
            "priority order:\n\n";
  prompt << "1. **Daily logs** (`logs/YYYY/MM/YYYY-MM-DD.md`) if present — "
            "these are the append-only stream\n";
  prompt << "2. **Existing memories that drifted** — facts that contradict "
            "something you see in the codebase now\n";
  prompt << "3. **Transcript search** — if you need specific context, grep "
            "the JSONL transcripts for narrow terms\n\n";
  prompt << "Don't exhaustively read transcripts. Look only for things you "
            "already suspect matter.\n\n";
  prompt << "## Phase 3 — Consolidate\n\n";
  prompt << "For each thing worth remembering, write or update a memory file "
            "at the top level of the memory directory. Use the memory file "
            "format and type conventions from your system prompt's auto-memory "
            "section — it's the source of truth for what to save, how to "
            "structure it, and what NOT to save.\n\n";
  prompt << "Focus on:\n";
  prompt << "- Merging new signal into existing topic files rather than "
            "creating near-duplicates\n";
  prompt << "- Converting relative dates (\"yesterday\", \"last week\") to "
            "absolute dates so they remain interpretable after time passes\n";
  prompt << "- Deleting contradicted facts — if today's investigation "
            "disproves an old memory, fix it at the source\n\n";
  prompt << "## Phase 4 — Prune and index\n\n";
  prompt << "Update `" << kEntrypointName << "` so it stays under "
         << kMaxEntrypointLines
         << " lines AND under ~25KB. It's an **index**, not a dump — each "
            "entry should be one line under ~150 characters: `- [Title](file.md) "
            "— one-line hook`. Never write memory content directly into it.\n\n";
  prompt << "- Remove pointers to memories that are now stale, wrong, or "
            "superseded\n";
  prompt << "- Demote verbose entries: if an index line is over ~200 chars, "
            "it's carrying content that belongs in the topic file — shorten "
            "the line, move the detail\n";
  prompt << "- Add pointers to newly important memories\n";
  prompt << "- Resolve contradictions — if two files disagree, fix the "
            "wrong one\n\n";
  prompt << "---\n\n";
  prompt << "Return a brief summary of what you consolidated, updated, or "
            "pruned. If nothing changed (memories are already tight), say so.";

  if (!extra.empty()) {
    prompt << "\n\n## Additional context\n\n" << extra;
  }

  return prompt.str();
}

}  // namespace memory
}  // namespace agent
