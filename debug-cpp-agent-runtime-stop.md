[OPEN] cpp-agent runtime stop

# Debug Session

- Session ID: `cpp-agent-runtime-stop`
- Scope: compare `jianlai_graph`, `jianlai_graph1`, `jianlai_graph-2` logs and latest change notes
- Status: fix implemented, awaiting user-side replay

# Initial Hypotheses

1. Continuation and task-closing rules in `cpp-agent` still mis-handle long-running real-project workflows, so the run exits before producing a verified final result.
2. The agent misreads tool outputs or project state, incorrectly concludes that work is complete, and stops without finishing tests or validation.
3. Large-output or multi-file real-project runs still hit a token/context or truncation boundary, causing the session to break off mid-verification.
4. The latest prompt / policy changes improved task continuation partially, but a missing guard in query-loop or runtime policy still allows premature termination after a partial milestone.
5. The issue is not only prompt-level; there is also a state persistence or completion-detection bug across turns in the `cpp-agent` runtime.

# Evidence Log

- `jianlai_graph`: write-only behavior. Verification nudges appeared, but the run kept creating files and never entered a stable run-check-fix loop.
- `jianlai_graph1`: runtime verification happened, but the session collapsed into repeated validator retries around the same `AttributeError` and terminated with `validator_retry_limit`.
- `jianlai_graph-2`: the session improved further and reached quick-run plus output inspection, but after discovering empty outputs it spent 12 consecutive turns on `Read`/`Glob`/`Grep` and was hard-stopped by `excessive_exploration`.
- The latest failure is therefore no longer "missing verification prompt". The real remaining bug is "after verification reveals a problem, exploration can continue too long and the runtime hard-terminates before the agent takes a concrete repair action."

# Confirmed Root Cause

1. `QueryLoop` treated prolonged exploration as a hard-stop condition even when the model had already gathered enough runtime evidence and still needed one more forced action turn.
2. Write-action detection was inconsistent: `FileEdit` and `MultiEdit` were not counted by the shared write-tool classifier, which made loop control less reliable.
3. Prompt guidance told the model to verify and inspect, but there was no strong runtime recovery step that said "stop exploring and act now" before termination.

# Fix Summary

- Updated `QueryLoop` to classify `FileEdit` and `MultiEdit` as workspace write actions.
- Replaced the first excessive-exploration hard stop with a one-time forced-action nudge that tells the model to stop reading/searching and immediately edit code, run verification, or surface a blocker.
- Kept the hard stop as a fallback after the forced-action nudge to avoid infinite loops.
- Strengthened the workspace prompt so that, after a failing or suspicious run, the model is told not to spend many turns only exploring.

# Verification

- `cmake -S . -B build` succeeded.
- `cmake --build build --config Release --target agent_test_queryloop_bash` succeeded.
- `build\\Release\\agent_test_queryloop_bash.exe` exited with code `0`.
- A full `cmake --build build --config Release` still hits an existing linker lock on `build\\Release\\agent_cli.exe`, which is unrelated to this fix.
