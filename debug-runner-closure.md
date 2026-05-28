# Debug Session: runner-closure

Status: OPEN

## Scope
- Track the real runtime chain for effective system prompt injection.
- Explain why `agent_real_project_runner.exe` can persist `main-model.jsonl` requests but not a trustworthy final `snapshot/transcript`.
- Explain why `agent_model_stream_smoke_runner.exe` may emit partial stream events and heartbeats without a full end summary.

## Falsifiable Hypotheses
1. A later runtime step rewrites or prepends prompt text after `BuildWorkspaceSystemPrompt()` and `QueryEngine::SetSystemPrompt()`.
2. The real runner persists request logs early, but the loop never reaches the final success/stop branch, so final snapshot persistence is skipped or captures an in-flight state.
3. The smoke runner receives a terminal event, but runner-side event accounting or output flushing exits before the summary is emitted.
4. The long prompt keeps the loop alive because planner/validator/continuation logic keeps generating more turns after the required directory read and file read are already complete.

## Evidence Log
- Prompt source confirmed: `main-model.jsonl` shows the effective prompt now comes from `BuildWorkspaceSystemPrompt()` and contains the workspace-first + Windows guidance text. No later overwrite was observed.
- Prompt chain refined: the real runtime `systemPrompt` is `BuildWorkspaceSystemPrompt(...)` plus `MemoryIndex::BuildSystemPromptInjection(...)`; the extra prompt body after the workspace rules comes from memory injection, not from a runner-side fallback to `AgentConfig::FromDefaults()`.
- Smoke runner confirmed: `agent_model_stream_smoke_runner.exe` emits terminal `stop_reason` events and `smoke_done=true`; earlier missing summaries were due to debug-server interference and incomplete stdout capture, not missing model terminal events.
- Real runner root cause #1: `GlobFiles()` did not support recursive `**` patterns, so requests like `src/**/*.cpp` incorrectly returned `(no matches)`, causing repeated exploration retries and forced continuation loops.
- Real runner root cause #2: runs that chose Bash listing commands (`ls`, `dir`, `Get-ChildItem`) were much slower and less stable than runs that used `Glob` + `Read`.
- Real runner closure confirmed: session `build\\runner-validation-session-runner-closure-5` completed with `loop_completed_reason=completed`, `run_finished=true`, `run_ok=true`, `message_count=8`, and a full `transcript.jsonl`.
- Runner summary bug confirmed: the integration runner parsed `turn_count` from `snapshot.pb`, so the stdout summary showed an untrustworthy `turn_count=-1` even when `snapshot.txt` was correct.
- Metadata bug confirmed: non-watchdog runner paths left `SessionMetadata.id` empty and never incremented `SessionMetadata.turnCount`, so `snapshot.txt` could show `session_id=` and `turn_count=0` even when `main-model.jsonl` and `transcript.jsonl` had already advanced.
- Post-fix validation confirmed: session `build\\runner-validation-session-runner-closure-7` now ends with `loop_completed_reason=completed`, `run_finished=true`, `run_ok=true`, `turn_count=1`, non-empty `session_id`, and a full `transcript.jsonl`.

## Next Steps
1. Keep debug artifacts until user confirms the fix is accepted.
2. Optionally clean up debug instrumentation and local debug server after confirmation.

## Fix Summary
- Added QueryLoop runtime instrumentation around model-call entry/return and no-tool continuation decisions.
- Fixed `Glob` recursive matching so patterns like `src/**/*` and `src/**/*.cpp` return correct results.
- Hardened the shared runtime prompt to prefer `Glob` / `Read` / `Grep` over shell listing commands for discovery tasks.
- Updated `agent_real_project_runner` to summarize `turn_count` from `snapshot.txt` instead of `snapshot.pb`.
- Initialized default session metadata in `SessionManager` and made `QueryEngine` increment `turnCount` even without a watchdog, so runner snapshots now carry a real `session_id` and `turn_count`.
