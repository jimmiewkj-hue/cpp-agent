# Debug Session: jianlai-runner-loop

Status: OPEN

## Scope
- Read all runtime logs under `G:\downloads\jianlai-graph\.cpp-agent`.
- Locate the exact time/context where LLM-planned execution terminated before finishing.
- Explain why queryloop stopped, why repeated questioning happened, and where `cpp-agent` diverges from `local-ace`.

## Falsifiable Hypotheses
1. `QueryLoop` exits through a limit branch such as `max_turns`, `wall_clock_budget_exceeded`, duplicate-tool-loop protection, or stop-hook prevention before the plan is complete.
2. Session snapshot / transcript / restore metadata is incomplete or stale, so resumed turns lose progress and re-ask the same unfinished question.
3. Tool failures or partial tool results are not translated into an actionable continuation path, causing the model to repeat planning instead of progressing.
4. Compared with `local-ace`, `cpp-agent` is missing recovery or continuation logic in flow control, resource budgeting, session management, or error handling.

## Latest Evidence
- `session_validation_after_fix_3` eventually reached `loop_completed_reason=wall_clock_budget_exceeded` and `run_finished=true`; the earlier "user-only snapshot" observation was taken before the run finished.
- `real_project_runner.cpp` was hard-coded to `SetMaxTurns(30)` and `SetWallClockBudgetMs(5 * 60 * 1000)`, while `src/app/main.cpp` runs with `SetMaxTurns(80)` and `SetWallClockBudgetMs(10 * 60 * 1000)`.
- `local-ace` uses higher-level budget awareness (`task_budget` / continuation-oriented query flow), while `cpp-agent`'s real runner previously stopped after one budget-limited turn with no automatic continuation.

## Applied Fix Direction
- Expose a `QueryEngine` helper that removes the trailing wall-clock timeout meta message so a follow-up turn can resume from the last real working state.
- Update `agent_real_project_runner` to align defaults with the main app, support env overrides for runner budgets, and automatically continue across `wall_clock_budget_exceeded` until a segment limit is hit.
