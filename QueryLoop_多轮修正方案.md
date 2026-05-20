# `cpp-agent` QueryLoop 多轮修正方案

## 1. 问题定义

`cpp-agent` 当前的 `QueryLoop` 已经有 `while (!state.completed)` 外壳，但实际行为仍然接近“单轮问答器”，无法像 `local-ace` 的 `queryLoop()` 那样在以下场景稳定续轮：

1. 首轮仅给出计划、分解步骤、下一步意图，但还没有真正执行工具。
2. 首轮产生了工具调用，工具结果注入后没有稳定进入下一轮推理。
3. 校验层要求 `retry_from_tools` 后，没有把“上一轮回答 + tool_result + validator guidance”重组进下一轮输入。
4. 命中 `prompt too long`、`max_output_tokens`、`token budget`、`stop hook blocking` 时，没有统一的“注入 meta 消息并强制续轮”机制。
5. `MAIN_MODEL`、`VALIDATOR_MODEL`、`FALLBACK_MODEL` 的配合关系没有落在主循环内部，导致模型切换和续轮状态割裂。

本次修正目标不是一次性把 `local-ace` 全量复杂度搬进来，而是先把“单次 QueryLoop 修正归零”，确保 `cpp-agent` 能稳定完成真实项目中的多轮执行。

## 2. 现状证据

### 2.1 `cpp-agent` 真实 session 证据

`G:\downloads\jianlai-graph\.cpp-agent\session\snapshot.txt` 显示：

- `turn_count=1`
- `message_count=2`
- 仅有一条用户消息和一条 assistant 计划性回答

`G:\downloads\jianlai-graph\.cpp-agent\session\transcript.jsonl` 显示 assistant 首轮只输出了“8 个步骤 + 下一步要查看目录”，但没有再次进入下一轮，也没有工具调用或 follow-up 消息。

这说明问题不是外层没有 `while`，而是“无工具调用路径下过早终止”。

### 2.2 当前代码根因

#### 根因 A: 无工具调用时直接完成

`src/core/QueryLoop.cpp` 当前在 `ModelCall` 和 `Validator` 分支里，只要 `state.toolUseBlocks.empty()`，就会进入：

1. `HandleMissingExpectedToolUse()`
2. `ApplyStepTerminate()`
3. `state.completed = true`

其中 `ApplyStepTerminate()` 的当前语义是：

- 把 `assistantMessages` 持久化到 `ctx.messages`
- 清掉自动压缩标记
- 直接把 `state.completed = true`
- `terminalReason = "completed"`

也就是说，只要本轮没有 `tool_use`，循环就被定义为结束，而不是“检查是否应该强制续轮”。

#### 根因 B: 缺少 `local-ace` 式的 no-tool continuation 链

`local-ace` 在“本轮无工具调用”时并不会立刻结束，而是依次检查：

1. `validator_retry`
2. `prompt-too-long` 恢复
3. `max-output-tokens` 扩容/续轮
4. `stop hooks`
5. `token budget`
6. 是否真正 `completed`

`cpp-agent` 虽然已经有 `Handle413Recovery()`、`HandleMaxOutputTokens()`、`HandleTokenBudget()`、`ExecuteStopHooks()`，但缺少一个统一的 no-tool 分支调度器，更缺少“强制续轮”策略。

#### 根因 C: 下一轮消息组织不完整

`local-ace` 的关键行为是：

`next.messages = [...messagesForQuery, ...assistantMessages, ...toolResults, ...followups]`

而 `cpp-agent` 当前多处 `continue` 只是在 `ctx.messages` 上做局部 push，然后重新进入 `ModelCall`。这会带来两个问题：

1. 各类 synthetic/meta 消息并未按统一顺序组织。
2. validator、tool result、stop hook blocking、budget nudge 的拼装顺序不稳定。

#### 根因 D: 工具执行后的续轮语义含混

`ApplyStepRunTools()` 的返回值 `shouldContinue` 当前只表达“有错误则回到 `ModelCall`”，而不是表达：

- 本轮工具执行结束，必须把 tool_result 喂给下一轮；
- 权限拒绝/Ask/只读并发/写工具串行后，如何稳定落到下一轮；
- 何时是“本轮完成”，何时是“继续同一任务的下一轮”。

虽然 `ToolOrchestrator::Execute()` 已经具备按批次执行、根据 `readOnlyHint` 区分并发安全批次和串行批次的基础能力，但 `QueryLoop` 没有把这个能力和“下一轮消息重组”绑定起来。

#### 根因 E: `fallback model` 没有真正进入主循环

`QueryEngine::HandleFallback()` 存在，但当前未接入 `RunTurn()` 主路径。结果是：

1. `MAIN_MODEL` 在 `QueryLoop` 中调用。
2. `VALIDATOR_MODEL` 在 side query 中调用。
3. `FALLBACK_MODEL` 只是配置存在，并未像 `local-ace` 那样成为主循环内部的重试策略。

这会导致“模型切换”和“续轮状态”分离，难以实现一致的恢复语义。

## 3. 修正原则

本次只解决 `cpp-agent` 的“单次 QueryLoop 修正归零”，按以下原则实施：

1. 保留 `ctx.messages` 作为当前最小改动下的历史主存储，不强行重构整个 `QueryEngine`。
2. 在 `QueryLoop` 内新增“本轮输入视图”和“下一轮 follow-up 注入”能力，补齐 `local-ace` 的核心语义。
3. 先完成多轮稳定性，再逐步把命令队列、记忆预取、技能附件、工具摘要做成扩展点。
4. 所有“强制续轮”都必须通过显式 meta message 落入 `ctx.messages`，不能靠隐式状态跳转。
5. 所有 synthetic 消息都必须写入 transcript，确保 session 可恢复、可对比、可调试。

## 4. 必须落地的结构改造

### 步骤 1: 扩展 `QueryLoopInternalState`

先把状态补齐，否则后续所有续轮都只能靠临时变量拼接。

建议新增字段：

- `std::vector<Message> messagesForTurn`
- `std::vector<Message> pendingFollowupMessages`
- `bool forceContinuation = false`
- `std::string forceContinuationReason`
- `bool stopHookActive = false`
- `std::string activeModel`
- `int nextTurnCount = 0`

字段职责：

- `messagesForTurn`: 当前轮真正送进主模型的消息视图，不直接污染完整历史。
- `pendingFollowupMessages`: validator/budget/stop hook/max token 等注入到下一轮的 meta 消息。
- `forceContinuation`: 当前轮即使无工具调用，也必须继续。
- `forceContinuationReason`: 记录原因，便于日志和测试断言。
- `stopHookActive`: 对齐 `local-ace` 的 stop hook 状态透传。
- `activeModel`: 明确当前轮实际调用的是主模型还是 fallback 模型。

完成标准：

- `QueryLoop.h` 状态字段补齐。
- 所有 `continue` 站点不再依赖隐式上下文。

### 步骤 2: 引入“本轮消息视图”而不是直接改写 `ctx.messages`

当前 `ApplyStepBudget/Snip/Microcompact/Collapse/Autocompact` 都直接改写 `ctx.messages`，这与 `local-ace` 的 `messagesForQuery` 语义不一致。

修正方式：

1. 每轮开始先执行：
   - `state.messagesForTurn = BuildMessagesForTurn(ctx, state);`
2. 压缩链统一作用于 `state.messagesForTurn`：
   - tool result budget
   - snip
   - microcompact
   - collapse
   - autocompact
3. 只有明确需要提交的压缩结果，才回写 `ctx.messages`。

最低可行实现：

- 新增 `BuildMessagesForTurn()`，先返回 `ctx.messages` 的副本。
- 所有 `ApplyStep*Compact` 改为优先处理 `state.messagesForTurn`。
- `ApplyStepModelCall()` 改为消费 `state.messagesForTurn`。

这一步的目的不是一次到位复刻 `getMessagesAfterCompactBoundary()`，而是先建立“本轮输入视图”和“完整历史”分离的骨架。

### 步骤 3: 用统一的 no-tool continuation 调度器替换 `ApplyStepTerminate()`

删除当前“无工具就 completed”的硬编码逻辑，新增：

- `bool QueryLoop::HandleNoToolContinuation(QueryLoopContext& ctx, QueryLoopInternalState& state);`

执行顺序必须固定为：

1. `validator_retry`
2. `prompt-too-long` 恢复
3. `max-output-tokens` 恢复
4. `api_error` 终止
5. `stop hooks`
6. `token budget`
7. `missing expected tool use`
8. `force continuation`
9. `completed`

处理语义：

- 只要命中任一“需要继续”的条件，就把 follow-up 消息写入 `ctx.messages` 和 transcript，设置 `state.stage = QueryStage::ToolResultBudget`，然后 `continue`。
- 只有全部都不命中，才真正 completed。

这一步是本次修复的核心。

### 步骤 4: 扩展“强制续轮”判定，不再只盯写文件

当前 `HandleMissingExpectedToolUse()` 只覆盖“说要写文件但没发工具”的情况，远远不够。

需要新增：

- `bool AssistantIntendsFurtherExecution(...)`
- `bool ShouldForceContinuation(...)`

至少覆盖以下信号：

1. assistant 明确说“下一步/让我先/接下来/I will/let me”，但没有 `tool_use`
2. 用户请求的是“真实修改、真实分析、真实测试”，assistant 却只输出方案/步骤清单
3. assistant 承诺查看目录、读取文件、运行测试、创建文件，但没有实际工具调用
4. validator 认为“最终回答与 execution evidence 不一致”

注入消息示例：

`[Continue] Do not stop at planning. Start the next concrete action now. If inspection or file changes are required, emit the appropriate tool call immediately.`

这类消息必须进入 `ctx.messages`，并在 transcript 中可见。

### 步骤 5: 重写 validator retry 的消息组织

当前 `ApplyStepValidator()` 在 `retry_from_tools` 时只是把 guidance push 到 `ctx.messages`，但没有统一保证：

- assistant draft 已持久化
- blocked tool synthetic result 已持久化
- guidance 已持久化
- 下一轮能按顺序看到这些消息

修正要求：

1. 如果 validator 产生 `corrected_text`，先原地修正 `assistantMessages`
2. 如果 validator block 了工具，生成 synthetic `tool_result`
3. 如果 `retry_from_tools`：
   - 先将 `assistantMessages` push 到 `ctx.messages`
   - 再 push synthetic `tool_result`
   - 再 push validator guidance meta message
   - 全部 persist
   - 清空当前轮临时状态
   - 回到 `ToolResultBudget`

下一轮必须看到：

`历史消息 + 本轮 assistant draft + synthetic tool_result + validator follow-up`

### 步骤 6: 规范工具执行后的续轮

`ApplyStepRunTools()` 需要从“执行工具”升级为“执行工具并为下一轮组织输入”。

#### 6.1 权限判定

继续沿用现有 `canUseTool(block, decisionMessages)` 机制，但要求：

1. 权限拒绝生成的 `tool_result` 也算 execution evidence。
2. Ask/deny/error 不能绕过下一轮。
3. 非只读工具必须串行，且每个工具结果都进入 `accumulatedMessages`，供后续工具权限判定使用。

#### 6.2 并发/串行语义

当前 `ToolOrchestrator::Execute()` 已经有 `PartitionToolCalls()` 基础能力，应保持：

- 只读工具：并发安全批次执行
- 非只读工具：串行执行

本次不要改掉这个方向，但要补三点：

1. 在 `QueryLoop_多轮修正方案` 验收中验证只读并发/写工具串行确实生效。
2. 所有工具结果按 assistant tool_use 顺序注入 `ctx.messages`。
3. 工具执行完成后，不论是否报错，只要已经生成了 tool_result，都默认走下一轮；不要直接当作整轮完成。

#### 6.3 `shouldContinue` 语义收敛

建议把 `ApplyStepRunTools()` 返回值语义改成：

- `true`: 工具执行后需要立刻进入下一轮
- `false`: 当前任务终止

并把默认行为改成：

- 只要执行过任何 tool_result，就返回 `true`
- 仅在 abort / fatal stop / plan mode 明确不需要续轮时返回 `false`

这样 `RunTools` 分支可以直接表达：

- 有工具结果 -> 必续轮
- 没有工具结果 -> 再看是否终止

### 步骤 7: 把 `fallback model` 合并进主循环

`fallback model` 不能继续停留在 `QueryEngine::HandleFallback()` 这种循环外逻辑。

建议改造：

1. `ApplyStepModelCall()` 内部使用 `state.activeModel`
2. 先调用 `ctx.model`
3. 如果出现明确的 fallback 触发条件，再切到 `ctx.fallbackModel`
4. fallback 只影响主模型调用，不影响 validator side query

三模型分工必须固定：

- `MAIN_MODEL`: 主推理与工具决策
- `VALIDATOR_MODEL`: 只负责校验/纠错/阻断
- `FALLBACK_MODEL`: 仅当主模型失败时替代主模型继续主循环

不要出现以下错误模式：

- validator 跟着 fallback 走
- fallback 后丢失此前的 `transition reason`
- fallback 后忘记续轮上下文

### 步骤 8: 明确 413 / max tokens / token budget 的强制续轮协议

#### 8.1 `prompt too long`

保留当前两级恢复思路，但顺序必须固定：

1. `collapse drain retry`
2. `reactive compact retry`
3. 仍失败才终止

并补齐：

- 每次恢复都要向 `ctx.messages` 注入 recover note
- 每次恢复都要清空本轮 assistant/toolUse 临时状态
- 恢复后必须从 `ToolResultBudget` 重新进入新一轮

#### 8.2 `max_output_tokens`

严格按照 `local-ace` 的模式：

1. 先尝试把 `max_output_tokens` 从默认值升级到 `64k`
2. 升级后仍命中，再注入“继续，不要道歉，不要复述”的 meta 恢复消息
3. 最多重试 3 次

注意点：

- `cpp-agent` 当前 `kEscalatedMaxTokens = 65536` 已存在，但并未真正传入 `ModelClient::StreamResponse()`，因为接口没有 `maxOutputTokensOverride`
- 本次必须同步扩展 `ModelClient` 的请求参数，否则状态字段只是摆设

#### 8.3 `token budget`

当前 `HandleTokenBudget()` 只是在极大阈值下插一条 nudge，但没有形成统一的 no-tool continuation 协议。

本次要求：

1. `token budget` 命中时也作为“强制续轮原因”
2. 注入 nudge 后回到 `ToolResultBudget`
3. transcript 中必须有 evidence，便于后续 session 对比

### 步骤 9: 预留“工具结果后处理”扩展点

`local-ace` 在工具执行后还有三类后处理：

1. 命令队列
2. 记忆预取/附件
3. 工具摘要

本次不要求把全部业务做满，但必须把阶段位留出来，至少形成一个统一入口：

- `PostToolTurnProcessing(ctx, state)`

最低要求：

1. 当前实现可以先是 no-op
2. 函数必须位于 `RunTools` 成功之后、下一轮 `ToolResultBudget` 之前
3. 所有未来的 attachment/tool summary/memory injection 都从这里进入 `ctx.messages`

这样才能保证下一轮消息组织稳定，而不是散落在不同 `continue` 分支里。

## 5. 代码修改顺序

必须严格按下面顺序做，不能跳步。

### Step 0: 建立基线

执行项：

1. 保存当前 `G:\downloads\jianlai-graph\.cpp-agent\session\snapshot.txt`
2. 保存当前 `transcript.jsonl`
3. 记录现象：
   - 只有第一轮
   - assistant 给计划但无后续

完成标准：

- 拿到修复前证据，便于后面对比“是否真的变成多轮”

### Step 1: 扩状态与辅助函数

改动文件：

- `src/core/QueryLoop.h`
- `src/core/QueryLoop.cpp`

执行项：

1. 扩展 `QueryLoopInternalState`
2. 新增统一消息持久化 helper
3. 新增 `BuildMessagesForTurn()`
4. 新增 `EnqueueFollowupAndContinue()` 类 helper

完成标准：

- 所有续轮站点都走统一 helper

### Step 2: 分离 `messagesForTurn`

改动文件：

- `src/core/QueryLoop.cpp`

执行项：

1. 压缩链改操作 `state.messagesForTurn`
2. `ApplyStepModelCall()` 改用 `state.messagesForTurn`
3. 确保 `ctx.messages` 只存历史，不作为本轮临时工作区

完成标准：

- 本轮输入和完整历史分离

### Step 3: 重写无工具分支

改动文件：

- `src/core/QueryLoop.cpp`

执行项：

1. 删除当前 `ApplyStepTerminate()` 的直接完成语义
2. 新增 `HandleNoToolContinuation()`
3. 接入 `force continuation`

完成标准：

- 首轮只有计划时，不再直接 completed

### Step 4: 修 validator retry 和 synthetic evidence

改动文件：

- `src/core/QueryLoop.cpp`

执行项：

1. 统一 blocked tool synthetic result 的注入和持久化
2. `retry_from_tools` 时统一按顺序组织下一轮消息
3. 保证 guidance 可进入 transcript

完成标准：

- validator 能真正触发第二轮，而不是只写一条散乱 meta 消息

### Step 5: 修工具执行后的必续轮逻辑

改动文件：

- `src/core/QueryLoop.cpp`
- 必要时 `src/tools/ToolOrchestrator.cpp`

执行项：

1. 明确 read-only 并发、write 串行语义
2. 工具结果按顺序落入 `ctx.messages`
3. 默认“只要出现 tool_result 就必续轮”

完成标准：

- assistant -> tool_use -> tool_result -> 下一轮 assistant 闭环跑通

### Step 6: 接入 fallback 与 max tokens override

改动文件：

- `src/core/QueryLoop.cpp`
- `src/api/ModelClient.h`
- `src/api/ModelClient.cpp`

执行项：

1. 给主模型调用增加可选 `maxOutputTokensOverride`
2. 把 fallback 重试下沉到 QueryLoop
3. 保持 validator model 独立

完成标准：

- `MAIN_MODEL` / `VALIDATOR_MODEL` / `FALLBACK_MODEL` 职责清晰且可复现

### Step 7: 插入 post-tool 扩展点

改动文件：

- `src/core/QueryLoop.cpp`

执行项：

1. 增加 `PostToolTurnProcessing()`
2. 当前先可为空实现
3. 保证插入时机固定

完成标准：

- 后续命令队列/记忆/工具摘要有稳定挂载点

## 6. 建议的关键函数设计

### 6.1 统一续轮 helper

建议新增：

```cpp
bool QueryLoop::ContinueWithFollowup(
    QueryLoopContext& ctx,
    QueryLoopInternalState& state,
    const std::vector<Message>& followups,
    TransitionReason reason);
```

职责：

1. 先把本轮 `assistantMessages` 写入历史
2. 再写入 follow-up 消息
3. 全部 persist
4. 清空 turn-local 状态
5. `state.stage = QueryStage::ToolResultBudget`
6. 重置必要的恢复计数器

### 6.2 无工具强制续轮判定

建议新增：

```cpp
bool QueryLoop::ShouldForceContinuation(
    const QueryLoopContext& ctx,
    const QueryLoopInternalState& state) const;
```

优先命中以下文本特征：

- “让我先”
- “下一步”
- “接下来”
- “我先查看”
- “I will”
- “Let me”
- “Next, I will”

并结合以下业务条件：

- 用户请求真实工作目录操作
- 当前无 tool_use
- 当前无最终交付物

### 6.3 下一轮消息组织 helper

建议新增：

```cpp
void QueryLoop::AppendTurnArtifacts(
    QueryLoopContext& ctx,
    const std::vector<Message>& assistantMessages,
    const std::vector<Message>& toolResults,
    const std::vector<Message>& followups);
```

注入顺序必须固定：

1. `assistantMessages`
2. `toolResults`
3. `followups`

## 7. 测试方案

本次修复必须同时做单元测试、集成测试、真实工作目录回归，任何一层没过都不能算“归零”。

### 7.1 单元测试

建议新增/扩展测试：

1. `test_queryloop_no_tool_plan_forces_continuation`
   - 首轮回答只有计划，无工具
   - 断言不会直接 `completed`
   - 断言 follow-up 被注入

2. `test_queryloop_validator_retry_reenters_loop`
   - validator 返回 `retry_from_tools`
   - 断言下一轮输入包含 assistant draft + guidance

3. `test_queryloop_tool_results_trigger_next_turn`
   - 首轮工具调用
   - 工具结果落入历史
   - 下一轮再次 `ModelCall`

4. `test_queryloop_stop_hook_blocking_continues`
   - stop hook 生成 blocking error
   - 断言 blocking error 写入历史并续轮

5. `test_queryloop_max_output_tokens_escalate_then_retry`
   - 首次命中 max tokens
   - 先升级 64k
   - 再次命中时注入 resume meta

6. `test_tool_orchestrator_readonly_parallel_write_serial`
   - 构造多个只读和写工具
   - 验证批次顺序与上下文可见性

### 7.2 集成测试

增加一个最小多轮 stub 模型流：

第一轮：

- assistant 文本: “我先检查目录并读取配置”
- 无工具

第二轮：

- assistant `tool_use(Read/LS)`

第三轮：

- assistant 基于 tool_result 输出最终答案

断言：

1. 循环至少发生两次
2. transcript 中存在 follow-up meta message
3. tool_result 出现在第二轮前
4. 最终 `terminalReason = completed`

### 7.3 真实工作目录回归

目标工作目录：

- `G:\downloads\jianlai-graph`

目标验证点：

1. 使用与失败样例相同或等价的真实用户需求
2. 首轮如果只给出计划，必须自动发起第二轮，而不是结束
3. 第二轮必须出现真实工具调用
4. 工具结果必须写入 session transcript
5. 至少完成“读取目录/读取关键文件/形成基于执行证据的下一步”这条闭环

建议验收证据：

- `G:\downloads\jianlai-graph\.cpp-agent\session\transcript.jsonl`
- `G:\downloads\jianlai-graph\.cpp-agent\session\snapshot.txt`

修复成功的最低标准：

- `snapshot.txt` 中 `turn_count > 1`
- `transcript.jsonl` 中存在：
  - 第一轮计划性回答
  - follow-up meta 消息
  - 第二轮 tool_use
  - tool_result
  - 第二轮或第三轮 assistant 基于 execution evidence 的继续回答

## 8. 验收红线

以下任一项未满足，视为修复失败：

1. 首轮无工具仍然直接 completed
2. validator retry 只写 guidance，不真正续轮
3. tool_result 没有进入下一轮上下文
4. fallback model 仍然停留在循环外部
5. `maxOutputTokensOverride` 只有状态没有实际请求参数
6. 真实工作目录回归后 `turn_count` 仍然等于 1

## 9. 实施建议

为了控制风险，建议拆成两次提交：

### 提交 1: 先打通多轮主干

只做：

1. 状态扩展
2. `messagesForTurn`
3. no-tool continuation
4. validator retry
5. tool_result -> next turn

目标：

- 真实任务从单轮变多轮

### 提交 2: 再补模型恢复与扩展点

再做：

1. fallback 内聚到 QueryLoop
2. `maxOutputTokensOverride` 真正传参
3. post-tool 扩展点
4. 更多回归测试

目标：

- 把多轮执行从“可用”提升到“稳定”

## 10. 本次修复的最小完成定义

只要同时满足以下四条，就可以判定“单次 QueryLoop 修正归零”：

1. 首轮只有计划时不会直接结束
2. 工具执行后一定进入下一轮
3. validator / stop hook / token budget / max tokens 都能通过 meta 消息强制续轮
4. 真实工作目录 session 显示 `turn_count > 1` 且 transcript 可见完整多轮链路

在这四条未全部满足之前，不要继续扩展别的能力。
