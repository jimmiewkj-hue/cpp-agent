import re, sys

path = r'G:\downloads\claude-code\yuanma-poxi\cpp-agent\src\core\QueryLoop.cpp'
with open(path, 'r', encoding='utf-8') as f:
    c = f.read()

orig_len = len(c)

# 1) Remove overly-broad English patterns from AssistantIntendsFurtherExecution
broad_en = [
    'let me check', 'let me try', 'let me read', 'let me look', 'let me see',
    'let me use', 'let me first', 'let me explore', 'let me examine',
    'i need to', 'i should check', 'i should read', 'i should try',
    'i should look', 'next step', 'proceed to', 'next, i will',
    'i will ', "i'll ", 'first, ', 'i should ',
    'check the project', 'inspect the project',
]
removed_count = 0
for p in broad_en:
    old = 'ContainsToken(lower, "' + p + '") ||'
    pattern = r' *[^\r\n]*' + re.escape(old) + r'[\r\n]+'
    new_c = re.sub(pattern, '', c, count=1)
    if new_c != c:
        c = new_c
        removed_count += 1

print(f'Removed {removed_count} broad English patterns')

# 2) Remove overly-broad Chinese patterns
broad_zh = ['\u6211\u5148\u67e5\u770b', '\u6211\u5148\u68c0\u67e5', '\u6211\u5148\u8bfb\u53d6', '\u5148\u67e5\u770b\u9879\u76ee']
for p in broad_zh:
    old = 'ContainsToken(original, "' + p + '") ||'
    pattern = r' *[^\r\n]*' + re.escape(old) + r'[\r\n]+'
    new_c = re.sub(pattern, '', c, count=1)
    if new_c != c:
        c = new_c
        removed_count += 1

print(f'Total removed: {removed_count} patterns')

# 3) Add text-only counter reset after tool execution in RunTools stage
old_run = '        bool hasToolResults = ApplyStepRunTools(ctx, state);\n        if (!hasToolResults) {'
new_run = '        state.consecutiveTextOnlyModelCalls = 0;  // tools produced, reset text-only counter\n        bool hasToolResults = ApplyStepRunTools(ctx, state);\n        if (!hasToolResults) {'
if old_run in c:
    c = c.replace(old_run, new_run, 1)
    print('Patch 3: Added text-only counter reset in RunTools stage')
else:
    print('WARNING: Patch 3 target not found')

# 4) Add early exit in HandleNoToolContinuation for consecutive text-only limit
# Find the function signature
sig_pattern = r'(bool QueryLoop::HandleNoToolContinuation\(QueryLoopContext&\s+ctx,\s+QueryLoopInternalState&\s+state\)\s*\{)'
match = re.search(sig_pattern, c)
if match:
    insert_after = match.group(0)
    early_exit = """
  // Align with local-ace: after kMaxConsecutiveTextOnly consecutive text-only
  // responses (no tool_use blocks), terminate instead of forcing continuation.
  // This prevents the infinite "let me write" planning loop seen in
  // jianlai-graph where the model repeatedly promises to write but never emits
  // a tool_use block.
  static const int kMaxConsecutiveTextOnly = 3;
  ++state.consecutiveTextOnlyModelCalls;
  if (state.consecutiveTextOnlyModelCalls >= kMaxConsecutiveTextOnly) {
    Message note;
    note.role = MessageRole::System;
    note.uuid = "text-only-loop-limit";
    note.isMeta = true;
    note.content.push_back(ContentBlock::MakeText(
        "[system] Terminating: model produced "
        + std::to_string(state.consecutiveTextOnlyModelCalls)
        + " consecutive text-only responses without any tool calls. "
        "The task may be incomplete."));
    AppendTurnArtifacts(
        ctx, state.assistantMessages, state.toolResultMessages, {note});
    state.assistantMessages.clear();
    state.toolResultMessages.clear();
    state.pendingFollowupMessages.clear();
    state.toolUseBlocks.clear();
    state.completed = true;
    state.terminalReason = "text_only_loop";
    return false;
  }
"""
    c = c.replace(insert_after, insert_after + early_exit, 1)
    print('Patch 4: HandleNoToolContinuation early exit added')
else:
    print('ERROR: Could not find HandleNoToolContinuation signature')
    sys.exit(1)

with open(path, 'w', encoding='utf-8') as f:
    f.write(c)
print(f'Done. File changed from {orig_len} to {len(c)} bytes')
