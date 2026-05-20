# [OPEN] stream-response-stall

## 现象

- 真实回归进入真实 workspace 成功
- 新 session 目录成功创建
- 运行器停在 `running_turn=true`
- 没有第一条 assistant 或 tool transcript 落盘

## 期望

- `HttpLlmClient::StreamResponse()` 能返回可解析的 JSON/SSE 内容
- `QueryLoop` 至少拿到首条 assistant/tool 事件
- session transcript 出现 assistant 或 tool 证据

## 初始假设

1. `SendHttpPost()` 在真实模型端点上阻塞，根本没有拿到 HTTP 响应体。
2. HTTP 已返回，但响应体很大或格式异常，卡在 `LooksLikeJsonPayload()` / SSE 解析链路。
3. 端点返回了错误或非预期 payload，但当前没有在 session/TUI 中暴露，导致看起来像“无输出卡死”。
4. `StreamResponse()` 已返回，但回调没有产出任何 `text_delta/tool_use/api_error` 事件，导致 `QueryLoop` 一直拿不到首条 assistant 消息。
5. WinHTTP 超时或流式读取配置不匹配当前本地模型服务，造成长时间等待而不是快速失败。

## 调试原则

- 先加运行时插桩，不改业务逻辑
- 优先抓请求开始/结束、耗时、响应体前缀、解析分支、事件计数
- 用最小真实 smoke runner 复现并对比日志

## 待办

- 给 `HttpLlmClient::StreamResponse()`、`SendHttpPost()`、SSE/JSON 解析关键分支加调试日志
- 给真实 runner 增加调试会话环境注入
- 复现真实模型调用并收集日志
- 基于证据判断根因，再做最小修复

## 当前证据

- `HttpLlmClient::StreamResponse()` 在真实环境中可以稳定拿到 `text/event-stream` 响应并完成 SSE 解析，不再是“首包未到”问题。
- OpenAI-compatible 的 validator / side-query 改成流式聚合后，不再卡在 `WinHttpReceiveResponse failed / 12002` 的非流式死等路径。
- `QueryLoop` 运行时插桩显示真实回合确实在持续推进：`ModelCall -> Validator -> RunTools -> ToolResultBudget` 多次往返。
- 初始真实阻塞链来自 Windows 下 `Bash` 工具实际走 `cmd.exe /c`，模型首个真实命令是 `ls -la`，导致工具结果反复失败并诱发重复工具调用。
- 已实施两处最小修复：
  - `ToolOrchestrator::ExecuteBash()` 切到 `powershell.exe -NoProfile -Command`，并对常见 `ls` 命令做 Windows 归一化。
  - 非零退出码现在会回写 `tool_result.isError=true`，避免把真实失败伪装成普通结果。
- post-fix 真实日志已证明主模型不再只会原地重复 `ls -la`：
  - 一度切换到 `Glob`
  - 后续无工具回答出现 `toolUseCount=0`
  - 强制续轮后已开始基于根目录结果规划“读取关键源码文件”

## 最新判断

1. “卡在 `running_turn=true` 后没有 assistant/tool 响应”这一层根因已被排除。
2. 当前剩余问题转为“真实回合收敛过慢”：
   - validator 经常返回长文本，但 `validation_json` 不稳定，`final_response_action` 常为空
   - 无工具回答分支会触发 forced continuation，导致回合继续推进而不是立刻落盘
3. 现阶段更像是 validator 策略/解析质量问题，而不是 HttpLlmClient 或 SSE/JSON 传输层问题。

## validation 对齐更新

- 已按 `local-ace` 对齐 `cpp-agent` validation layer 的 4 个关键点：
  - 只有显式配置 `AGENT_VALIDATOR_MODEL`/`CPP_AGENT_VALIDATOR_MODEL` 时才启用 validation，不再默认总开。
  - validation context 改成 `user_goal` / `assistant_text` / `assistant_tool_calls` / `relevant_tool_schemas` / `execution_evidence` 结构。
  - validator system prompt 改成与 `local-ace` 一致的 XML + `validation_json` 协议，支持 `text_correction.needed/corrected_text`、`tool_interventions`、`final_response_action`、`retry_guidance`。
  - `retry_from_tools` 改为在执行工具前直接续轮，并把 validator guidance 追加回总 `messages`。
- 已补单测覆盖：
  - validator `retry_from_tools` 会阻止原始工具执行并推进下一轮
  - validator `text_correction` 会改写最终 assistant 文本
- 已继续把 validator 请求参数向 `local-ace` 对齐：
  - OpenAI-compatible `SideQuery` 改成 `max_tokens=4096`
  - `SideQuery` 走专用 deterministic 流式请求，显式 `temperature=0`

## 最新真实回归结论

- validation 对齐后，真实 `.dbg` 日志已能多次看到：
  - `validator:parsed.hasValidationJson=true`
  - `finalResponseAction=approve`
  - 工具结果成功推进到下一轮
- 但在真实 `Qwen`/`gemma` validator 组合下，结构化返回仍不稳定：
  - 有时输出合规 `validation_json`
  - 有时仍直接输出自然语言总结
- 因此本轮结论是：
  - `cpp-agent` 的 validation layer 实现逻辑已基本与 `local-ace` 对齐
  - “最终 RunTurn 是否稳定结束并落盘” 的剩余瓶颈，已主要落到真实 validator 模型的结构化服从度，而不再是 `cpp-agent` 流程实现差异

## gemma validator benchmark

- 已新增独立 benchmark 程序 [gemma_validator_benchmark.cpp](file:///g:/downloads/claude-code/yuanma-poxi/cpp-agent/tests/integration/gemma_validator_benchmark.cpp)，专门测试 validator 的结构化返回稳定性。
- benchmark 维度：
  - 4 类 payload/case：`approve_tool`、`retry_from_tools`、`text_correction`、`block_write`
  - 5 类 template：`baseline_localace`、`strict_xml_contract`、`compact_skeleton`、`fewshot_guard`、`minimal_xml_only`
  - 指标：`has_validation_json`、`parsed_json`、`no_extra_text`、`final_response_action`、耗时、是否超时
- 当前探索结果：
  - `baseline_localace` 在最简单 `approve_tool` case 上偶尔能给出结构化 `approve`，但通常会夹带 XML 外自然语言解释。
  - 一旦进入更难的 `retry_from_tools` / `text_correction` / `block_write` case，`gemma` 很容易退化成自然语言总结，甚至直接在 `SideQuery` 上超时。
  - 更严格的模板（`strict_xml_contract`、`fewshot_guard`、`minimal_xml_only`）并没有明显提升结构化命中率，反而更容易触发 `WinHttpReceiveResponse failed`。
- 当前阶段性判断：
  - benchmark 已证明问题不只是"输出格式不稳"，还包括 `gemma` 在复杂 validator prompt 下的真实时延/吞吐问题。
  - 目前还没有找到一套让 `gemma` "必须稳定结构化返回"的 prompt/template。
  - 对 `gemma` 更现实的下一步可能不是继续堆 prompt，而是：
    1. 加二次重问/repair pass，把自然语言输出再压成 XML
    2. 引入更短、更离散的 per-action validator task，而不是一个大而全的综合 validator prompt
    3. 把 benchmark 结果按 run label 持久化，继续做多轮统计而不是单次 spot check

---

## 🏁 最终结论：Qwen 主模型 vs gemma validator 职责占比分析

### 实验证据汇总

#### 证据 A — gemma validator 独立结构化基准测试 (2026-05-20)

| Variant | structured | strict | expected_action |
|---|---:|---:|---:|
| V01_baseline_localace (单轮) | 1/4 | 0/4 | 1/4 |
| V06_twopass_repair (judge+repair) | 0/4 | 0/4 | 0/4 |

- **8 次运行中 7 次都产生了 `<validation_json>` 标签**（hasValidationJson=true）
- **但其中 4 次 parsedJson=false**：标签内的 JSON 不合法
- **其中 5 次 final_response_action 为空**：JSON 即使能解析，关键字段也缺失
- **two-pass repair 反而更差**（0/4 structured vs 1/4 baseline），且总时延 2-4x
- gemma 的典型失败模式：
  - 在 `<validation_json>` 内输出 `"action"` 字段而非 `"final_response_action"`
  - repair pass 把 JSON 内容包进了 XML 元素而非文本
  - 自然语言推理段落始终出现在 XML 标签之外

#### 证据 B — cpp-agent 真实工程 transcript（Qwen + gemma validator）

关键 transcript 片段（[session_qwen_only/transcript.jsonl](file:///g:/downloads/claude-code/yuanma-poxi/cpp-agent/.dbg/session_qwen_only/transcript.jsonl)）：

```
turn 1: Qwen → "Let me start by listing the root directory." + Bash(ls -la)
        gemma validator → hasValidationJson=true, finalResponseAction=""
        → tool executed, result returned

turn 2: Qwen → plans more + Bash(ls src/)
        gemma validator → hasValidationJson=true, finalResponseAction=""
        → tool executed

turn 3: Qwen → Bash(ls -la / | head -30) — Windows 上失败
        gemma validator → hasValidationJson=true, finalResponseAction=""
        → tool error returned

...30+ turns of planning-replanning-forcedContinuation...

最终状态：
- heartbeats=247 (~20分钟运行时间)
- turn_count=-1 (从未正常收敛)
- transcript_size=58084 bytes
- 大量 `forced-continuation` 注入后 Qwen 陷入重复规划循环
```

#### 证据 C — Qwen-only clean run（无 validator）

- turn_count=0, message_count=1（仅用户 prompt 落盘）
- RunTurn 在无 validator 时同样未收敛

### 核心结论

#### 1️⃣ `local-ace` 能稳定运行的关键要素（按重要性排序）

**A. Qwen 主模型贡献 ~95%+，是决定性因素**

`local-ace` 的成功不依赖于 gemma validator 的"结构化返回质量"，而是依赖：
- Qwen 自身的推理能力、工具调用能力、代码生成能力
- `local-ace` 预置的系统 prompt 和工具描述对 Qwen 做了充分调优
- Qwen 流式输出 → 工具执行 → 结果回传的闭环是**纯 Qwen 驱动**的

**B. gemma validator 的作用是"安全滤网"，不是"质量控制"**

gemma 在 `local-ace` 中的实际行为：
- `parseValidationResponse()` 在解析失败时静默降级（不抛异常，不阻止执行）
- 只有当 `final_response_action === 'retry_from_tools'` 时才触发重试
- gemma 结构化失败 → `finalResponseAction` 为空 → **行为等价于 `approve`**（原封不动放行 Qwen 输出）
- 因此 gemma 的"不稳定"在 `local-ace` 中被**优雅吸收**了

**C. `local-ace` 的 streaming + validation 架构天然容错**

关键代码在 [query.ts:L1060-L1179](file:///g:/downloads/claude-code/yuanma-poxi/local-ace/src/query.ts#L1060-L1179)：
```ts
// 流式阶段 suppress yield，主模型输出仅存内存
// validation 之后才 emit 到 transcript
if (config.gates.validationEnabled && assistantMessages.length > 0) {
    const validationResult = await validateAssistantDraft(...)
    // 解析失败 → validatorRetry 保持 false → 继续
    if (validationResult.finalResponseAction === 'retry_from_tools') {
        // 只有明确 retry 才回退
        continue
    }
}
// 校验后的结果才 yield（落盘到 transcript）
```

#### 2️⃣ `cpp-agent` 中"卡住"的真正根因

**不是 gemma validator 的结构化输出问题，而是三重放大效应：**

**根因 1：Forced Continuation 过于激进**

[QueryLoop.cpp:L746-L753](file:///g:/downloads/claude-code/yuanma-poxi/cpp-agent/src/core/QueryLoop.cpp#L746-L753)：
```cpp
bool QueryLoop::ShouldForceContinuation(...) const {
  if (state.forceContinuation) return true;
  if (!state.toolResultMessages.empty()) return true;
  if (!state.toolUseBlocks.empty()) return false;
  if (state.assistantMessages.empty()) return false;
  return AssistantIntendsWorkspaceWrite(state.assistantMessages) ||
         AssistantIntendsFurtherExecution(state.assistantMessages);
}
```

`AssistantIntendsFurtherExecution` 在 [QueryLoop.cpp:L258-L278](file:///g:/downloads/claude-code/yuanma-poxi/cpp-agent/src/core/QueryLoop.cpp#L258-L278) 匹配了大量中文"计划性"短语：
- `"让我先"` `"接下来"` `"下一步"` `"我先查看"`
- Qwen 的习惯性中文表达"让我先列出根目录"会**每次**触发这个检测

**根因 2：validator 失败 + forced continuation 形成正反馈环**

```
Qwen 计划性回答 → ForcedContinuation 触发 → 注入 "Start the next concrete action now"
→ Qwen 收到新消息后再次"计划" → ForcedContinuation 再次触发 → ...
```

Transcript 中 `forced-continuation` 出现了 **13+ 次**，且 Qwen 的回复从未真正脱离规划模式。

**根因 3：forced continuation 缺少上限**

`local-ace` 的 `needsFollowUp` 在下一轮会被自然消费；
`cpp-agent` 的 `ShouldForceContinuation` 没有计数器限制，可以无限触发。

### 3️⃣ gemma 在 cpp-agent 中"不稳定"而 local-ace 中"可用"的原因

| 维度 | local-ace | cpp-agent |
|---|---|---|
| validator 失败后果 | 静默降级为 approve，原样放行 | 触发复杂的 forcedContinuation 链 |
| 解析失败处理 | `try/catch` → 返回空 `ValidationResult` → 无副作用 | `try/catch` → 返回空，但后续 Handler 仍可能触发 |
| 额外延迟 | OpenAI `/v1/chat/completions` 单次请求 | 同样路径，但有后续 handler 开销 |
| 对 Qwen 行为的影响 | 透明（validator 失败 = 不存在） | 放大（validator 失败 + forcedContinuation 叠加） |

**结论：不是 gemma 在 local-ace 中更稳定，而是 local-ace 的架构对 gemma 的不稳定做了更好的容错。**

### 4️⃣ 解决方案（按优先级）

#### 方案 1（最小修复）：添加 forced continuation 上限

在 [QueryLoop.cpp](file:///g:/downloads/claude-code/yuanma-poxi/cpp-agent/src/core/QueryLoop.cpp) 中：
- 给 `QueryLoopInternalState` 添加 `forcedContinuationCount` 计数器
- 当计数超过阈值（建议 2-3 次）时，不再触发 forced continuation
- 直接终止回合，接受当前输出

#### 方案 2（架构对齐）：validator 失败默认 approve

在 `ApplyStepValidator` 中：
- 当 `final_response_action` 为空时，显式视为 `approve`
- 不再依赖后续 handler 去"猜"意图
- 行为与 `local-ace` 的 `parseValidationResponse` 一致

#### 方案 3（进阶）：减少 validator 调用频率

- 只在满足特定条件时调用 validator（如：tool 数量 > 0、回答长度 > 阈值）
- 纯文本回答可以跳过 validator（与方案 2 的 approve 默认一致）
