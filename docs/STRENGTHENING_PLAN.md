# cpp-agent 强化计划文档（逐模块修复指南）

> **文档用途**：本文件是给**中低级别 LLM / 工程师**逐项执行修复的工作手册。每个任务自包含：含问题、证据（`文件:行号`）、目标行为、精确改法、验收标准。任务间标注依赖关系，应按编号顺序执行。
>
> **对比基准**：local-ace（`G:\downloads\claude-code\yuanma-poxi\local-ace`，TypeScript，fork 自 Anthropic Claude Code）。
> **被强化对象**：cpp-agent（`G:\downloads\claude-code\yuanma-poxi\cpp-agent`，C++17）。
>
> **执行约定**：
> 1. 每个任务做完先跑该任务的"验收标准"，全绿才能标完成。
> 2. 不要跳级。前置任务未完成的，先做前置。
> 3. 改动必须保留原有注释风格（`// P0-XX: ...`），并在新增代码顶部加 `// STRENGTHEN-XX: <一句话说明>`。
> 4. 所有新增/修改的公开 API 必须同步更新对应 `.h` 文件注释。
> 5. 涉及多文件协同的改动，一次 commit 只做一个任务。

---

## 目录

| 模块 | 任务编号 | 优先级 | 标题 |
|---|---|---|---|
| 全局/构建 | T00 | P0 | 修复 POSIX 构建路径诚实性 |
| MCP 工具 | T01 | P0 | 接通 MCP 工具调用（`tools/call`） |
| QueryLoop | T02 | P0 | 落地统一 no-tool 续轮调度器 |
| QueryLoop | T03 | P0 | 分离 `messagesForTurn` 与 `ctx.messages` |
| QueryLoop | T04 | P0 | `maxOutputTokensOverride` 真正传入 ModelClient |
| QueryLoop | T05 | P1 | `fallback model` 内聚进主循环 |
| QueryLoop | T06 | P1 | 统一续轮重组 helper `ContinueWithFollowup` |
| 双模型 | T07 | P0 | 引入 `ValidatorTier` 能力分层触发 |
| 双模型 | T08 | P0 | 废弃硬替换，改结构化 patch 注入 |
| 双模型 | T09 | P0 | 动态有效率门控（自适应熔断） |
| 双模型 | T10 | P1 | 事前契约（execution_contract） |
| 双模型 | T11 | P2 | 并行预判校验（异步护栏） |
| 权限 | T12 | P1 | BashClassifier 接通 LLM 分类 |
| 权限 | T13 | P1 | 权限校验器按工具细分 |
| 工具 | T14 | P1 | 扩充工具集（移植 local-ace 工具） |
| 上下文 | T15 | P1 | 独立 reactiveCompact + cachedMCConfig |
| 记忆 | T16 | P2 | 补 skillSearch / teamMemorySync |
| Hooks | T17 | P2 | 事件广播系统 + 多执行器 + ssrfGuard |
| 沙箱 | T18 | P2 | 评估引入 OS 级沙箱 |
| 会话 | T19 | P2 | VCR 录像回放 |
| 并发 | T20 | P2 | 流式工具执行运行期开启 |
| 文档 | T21 | P2 | 同步 ARCHITECTURE_ANALYSIS.md parity 表 |

---

## T00 — 修复 POSIX 构建路径诚实性【P0】

**问题**：CMake 声明支持 POSIX（链接 libcurl + pthread），但核心源文件无条件 `#include <windows.h>`/`<winhttp.h>`，导致 Linux 构建根本编译不过。这是"宣称能力 ≠ 实装能力"的诚信问题。

**证据**：
- `CMakeLists.txt:145-150` POSIX 分支链接 libcurl + pthread
- `src/core/QueryLoop.cpp:15` 无条件 `#include <windows.h>`
- `src/tools/ToolOrchestratorNew.cpp:16-17` 无条件 winhttp
- `src/api/ModelClient.cpp` HTTP 全走 WinHttp
- `src/platform/PlatformPosix.cpp` 存在但只覆盖进程/文件，不覆盖 HTTP

**目标**：任选其一，**消除"虚假跨平台"**：
- **方案 A（推荐，工作量小）**：CMake 移除 POSIX 分支，`README.md` 顶部明确标注"当前仅支持 Windows，Linux 支持在路线图"，避免误导。
- **方案 B（工作量中）**：抽象 `HttpLlmClient` 接口，POSIX 下用 libcurl 实装，Windows 下保留 WinHttp，用 `#ifdef _WIN32` 切换。

**改法（方案 A）**：
1. `CMakeLists.txt`：删除 `if(NOT WIN32)` 链接 libcurl/pthread 的分支（行 145-150），改为 `message(FATAL_ERROR "cpp-agent currently supports Windows only. See docs/STRENGTHENING_PLAN.md T00.")` 当 `NOT WIN32` 时。
2. `README.md` 顶部加 `> ⚠️ 平台支持：仅 Windows。Linux/macOS 支持待 T00 方案 B 实装。`
3. 保留 `PlatformPosix.cpp` 文件但加 `// TODO(T00-B): libcurl HTTP 实装后启用`。

**验收标准**：
- [ ] 在 Linux 上 `cmake ..` 直接报清晰错误，而非编译到一半失败
- [ ] `README.md` 含平台声明
- [ ] `grep -r "winhttp" src/ | grep -v "_WIN32"` 为空（winhttp 必须被 `#ifdef` 包裹）

---

## T01 — 接通 MCP 工具调用（`tools/call`）【P0，最严重实装缺口】

**问题**：cpp-agent 的 MCP 子系统**只能列举工具/资源，不能真正调用工具**。MCP 工具名按 `mcp__{server}__{tool}` 规则注册，但 `ToolOrchestrator::ExecuteToolBlock` 的手写 switch 里**没有任何 `mcp__` 分支**，所有 MCP 工具调用命中 `"unknown tool"`（`ToolOrchestratorNew.cpp:298-301`）。同时 `McpClientManager` **缺少 `CallTool` 方法**——连底层 RPC `tools/call` 都没实装。

**证据**：
- `src/tools/ToolOrchestratorNew.cpp:214-302` — `ExecuteToolBlock` 的 if-else 链，无 `mcp__` 前缀匹配
- `src/tools/ToolOrchestratorNew.cpp:298-299` — `*error = "unknown tool: " + resolvedName`
- `src/mcp/McpClientManager.h:105-161` — `McpClientManager` 公开 API 只有 `RefreshTools/Prompts/Resources`、`ReadResourceFromTransport`、`FetchToolsForClient`，**没有 `CallTool`**
- `src/mcp/McpClientManager.cpp:2001-2006` — `BuildMcpToolName` 产出 `"mcp__" + server + "__" + tool`
- `src/tools/ToolOrchestratorNew.cpp:1884-1937` — `ExecuteListMcpResources`/`ExecuteReadMcpResource` 已正确接通，可作为集成模式参考

**目标行为**：
- 主模型发出 `mcp__github__create_issue` 工具调用 → 经 transport 发送 `tools/call` JSON-RPC → 返回结果注入 `tool_result` → 进入下一轮

**改法（分 3 步，严格按序）**：

### 步骤 1：给 `McpClientManager` 增加 `CallTool` 方法

在 `src/mcp/McpClientManager.h` 的 `McpClientManager` 类 public 区（约行 142 `ReadResourceFromTransport` 之后）新增：

```cpp
// STRENGTHEN-01: Invoke a tool on a connected MCP server via tools/call RPC.
// serverName: MCP server name (matches RegisterServer). toolName: tool name
// (NOT the mcp__-prefixed fully qualified name). argumentsJson: tool input
// as a JSON object string. Returns the result content; *error set on failure.
bool CallTool(const std::string& serverName,
              const std::string& toolName,
              const std::string& argumentsJson,
              std::string* resultJson,
              std::string* error);
```

在 `src/mcp/McpClientManager.cpp` 实装。**完全照抄 `ReadResourceFromTransport` 的结构**（它已经做了：找连接 → 构造 `McpTransportRequest` → `transport->Send` → 解析响应）。区别只是 `method = "tools/call"`，`paramsJson` 形如 `{"name":"<toolName>","arguments":<parsed arguments>}`。响应体形如 `{"result":{"content":[{"type":"text","text":"..."}]}}`，提取 `content` 数组的文本拼接返回。

### 步骤 2：在 `ExecuteToolBlock` switch 里加 `mcp__` 分支

在 `src/tools/ToolOrchestratorNew.cpp:214` 的 `ExecuteToolBlock` 函数体内，**在所有内建工具 if 之后、`unknown tool` 兜底之前**（即行 296 `WebSearch` 之后）插入：

```cpp
// STRENGTHEN-01: dispatch mcp__<server>__<tool> to McpClientManager::CallTool
if (resolvedName.rfind("mcp__", 0) == 0 && mcpClientManager_) {
  return ExecuteMcpTool(resolvedName, inputJson, maxResultSize, error);
}
```

### 步骤 3：新增 `ExecuteMcpTool` 方法

在 `ToolOrchestratorNew.cpp`（`ExecuteReadMcpResource` 之后，行 ~1937）新增：

```cpp
// STRENGTHEN-01: parse mcp__server__tool name, split args, call MCP server.
std::string ToolOrchestrator::ExecuteMcpTool(
    const std::string& fullyQualifiedName,
    const std::string& inputJson,
    int maxResultSize,
    std::string* error) const {
  // 1. 拆分 "mcp__server__tool" -> server, tool
  //    规则:去掉前缀 "mcp__",按第一个 "__" 拆分
  std::string body = fullyQualifiedName.substr(5);  // 去掉 "mcp__"
  auto sep = body.find("__");
  if (sep == std::string::npos) {
    if (error) *error = "malformed MCP tool name: " + fullyQualifiedName;
    return "Error: malformed MCP tool name";
  }
  std::string server = body.substr(0, sep);
  std::string tool = body.substr(sep + 2);

  // 2. MCP 工具的输入参数可能在 inputJson 的不同字段下
  //    local-ace 约定:整个 inputJson 就是 arguments 对象,直接透传
  std::string resultJson;
  std::string callError;
  if (!mcpClientManager_->CallTool(server, tool, inputJson,
                                   &resultJson, &callError)) {
    if (error) *error = callError;
    return "Error: MCP tool call failed: " + callError;
  }
  return TruncateResult(resultJson, maxResultSize);
}
```

并在 `src/tools/ToolOrchestrator.h` 的 private 区声明 `ExecuteMcpTool`。

**前置任务**：无。

**验收标准**：
- [ ] 新增单元测试 `tests/test_mcp_call_tool.cpp`：mock 一个 `McpTransport`，`Send` 收到 `method=="tools/call"` 时返回固定 JSON，断言 `CallTool` 返回该 JSON 的 content 文本
- [ ] 集成测试：起一个 echo MCP server（可复用 local-ace `services/mcp/InProcessTransport.ts` 思路写个 C++ stub），主模型调用其工具能拿到结果
- [ ] `grep "mcp__" src/tools/ToolOrchestratorNew.cpp` 能命中新增分支
- [ ] 手测：配置一个真 MCP server，让主模型调它的工具，transcript 里出现真实 `tool_result`

---

## T02 — 落地统一 no-tool 续轮调度器【P0，QueryLoop 核心病灶】

**问题**：`ApplyStepTerminate` 在"本轮无 tool_use"时直接 `state.completed = true`，导致**首轮主模型只输出计划/下一步意图就立即结束**，无法稳定进入第二轮。local-ace 有固定的续轮判定链。

**证据**：
- `src/core/QueryLoop.h:121` — 已声明 `HandleNoToolContinuation`（说明计划已部分落地，需核实完整度）
- `src/core/QueryLoop.cpp:3219` — `HandleNoToolContinuation` 存在，含 `kMaxValidatorRetryContinuations`
- `src/core/QueryLoop.cpp:3549` — 注释"when model produces no tool_use and no validator ..."
- 真实 session 证据见 `QueryLoop_多轮修正方案.md` 第 2.1 节：`turn_count=1`，assistant 只给计划无后续

**目标行为**：本轮无 tool_use 时，按**固定优先级链**逐一判定是否应强制续轮，仅当全部不命中才真正 `completed`：

```
1. validator_retry          (validator 请求 retry_from_tools)
2. prompt_too_long 恢复     (Handle413Recovery)
3. max_output_tokens 恢复   (HandleMaxOutputTokens)
4. api_error 终结           (致命 API 错误)
5. stop hooks               (ExecuteStopHooks 阻塞)
6. token budget             (HandleTokenBudget)
7. missing expected tool    (assistant 说"让我先..."却没发工具)
8. force continuation       (forceContinuation == true)
9. completed                (真正结束)
```

**改法**：

1. **核对 `HandleNoToolContinuation` 已覆盖全部 9 条**。打开 `src/core/QueryLoop.cpp:3219`，逐条核对。缺哪条补哪条。每条命中后必须：
   - 把对应 follow-up meta message 写入 `state.pendingFollowupMessages`
   - 设 `state.stage = QueryStage::ToolResultBudget`
   - `return true`（表示已续轮）
   - 唯一全部不命中才 `return false`（表示真完成）

2. **删除 `ApplyStepTerminate` 里的"无工具即完成"硬编码**。定位 `ApplyStepTerminate`（`QueryLoop.cpp:3165` 附近），把它的完成条件改为"仅当 `HandleNoToolContinuation` 返回 false 时才 `completed=true`"。

3. **新增 `ShouldForceContinuation` 文本特征判定**（对应第 7 条）。在 `QueryLoop.cpp` 新增：

```cpp
// STRENGTHEN-02: 检测 assistant 文本暗示将继续执行但未发工具
bool QueryLoop::ShouldForceContinuation(
    const QueryLoopContext& ctx,
    const QueryLoopInternalState& state) const {
  if (!state.toolUseBlocks.empty()) return false;        // 有工具不算
  if (state.assistantMessages.empty()) return false;
  // 取最后一条 assistant 的文本
  std::string text; /* 从 state.assistantMessages 提取 */
  std::string lower = ToLower(text);
  // 命中任一意图短语 + 用户请求是真实操作类任务 -> 强制续轮
  static const char* kIntents[] = {
    "让我先", "下一步", "接下来", "我将", "i will", "let me",
    "next, i", "first, let", "i'll"};
  for (auto p : kIntents) {
    if (lower.find(p) != std::string::npos) {
      // 还需判定用户原始请求是"真实操作"而非"纯问答"
      std::string goal = ToLower(/* ctx 里最后一条 user 消息 */);
      if (ContainsRealActionGoal(goal)) return true;
    }
  }
  return false;
}
```

**`ContainsRealActionGoal` 判定**：goal 含"修改/创建/运行/测试/实现/分析/fix/create/run/test/implement"等动词，且不含"解释一下/什么是/区别/why/what is"等纯问答词。

**前置任务**：无。

**验收标准**：
- [ ] 新增单元测试 `tests/test_queryloop_no_tool_plan_forces_continuation.cpp`：mock 主模型首轮只返回"让我先查看目录"，断言不 `completed`、follow-up 被注入
- [ ] 新增 `test_queryloop_validator_retry_reenters_loop`、`test_queryloop_stop_hook_blocking_continues`
- [ ] 真实工作目录回归（见修正方案文档第 7.3 节）：`turn_count > 1`，transcript 含完整多轮链路

---

## T03 — 分离 `messagesForTurn` 与 `ctx.messages`【P0】

**问题**：各 `ApplyStep*Compact` 直接改写 `ctx.messages`（历史主存储），导致"本轮临时工作区"和"完整历史"混在一起，状态割裂。local-ace 用独立的 `messagesForQuery`。

**证据**：
- `src/core/QueryLoop.h:32` — `QueryLoopInternalState` 已有 `messagesForTurn` 字段（计划已部分落地）
- `src/core/QueryLoop.h:136` — 已声明 `BuildMessagesForTurn`
- 但需核实 `ApplyStepBudget/Snip/Microcompact/Collapse/Autocompact` 是否已全部改作用 `state.messagesForTurn`

**目标行为**：
- 每轮开始：`state.messagesForTurn = BuildMessagesForTurn(ctx, state)`（先返回 `ctx.messages` 副本）
- 所有压缩步骤只作用 `state.messagesForTurn`
- 仅 `ApplyStepModelCall` 消费 `state.messagesForTurn`
- `ctx.messages` 只在明确需要提交压缩结果时才回写

**改法**：

1. **逐个核对 5 个压缩 ApplyStep**：`ApplyStepBudget`/`ApplyStepSnip`/`ApplyStepMicrocompact`/`ApplyStepCollapse`/`ApplyStepAutocompact`（`QueryLoop.cpp` 内）。凡签名是 `QueryLoopContext& ctx` 且直接改 `ctx.messages` 的，改为改 `state.messagesForTurn`。注意只读 `ctx.messages` 作为源。
2. **`ApplyStepModelCall`** 把传给 `modelClient_.StreamResponse` 的 messages 参数从 `ctx.messages` 改为 `state.messagesForTurn`。
3. **提交点**：只有 `ApplyStepAutocompact` 真正完成压缩后，才把 `state.messagesForTurn` 回写到 `ctx.messages`（加注释 `// 仅此处回写历史主存储`）。

**前置任务**：T02（先有续轮框架，再分离视图，否则状态更乱）。

**验收标准**：
- [ ] `grep "ctx.messages" src/core/QueryLoop.cpp` 在 ApplyStep* 函数体内不再出现写操作（只读允许）
- [ ] 单元测试：压缩触发后，`ctx.messages` 仅在提交点变化，中间步骤不污染

---

## T04 — `maxOutputTokensOverride` 真正传入 ModelClient【P0】

**问题**：`QueryLoopInternalState::maxOutputTokensOverride`（`QueryLoop.h:58`）是状态字段，但 `ModelClient::StreamResponse` 的接口**没有这个参数**，状态字段是摆设，`max_output_tokens` 升级到 64k 永不生效。

**证据**：
- `src/core/QueryLoop.h:58` — `int maxOutputTokensOverride = 0;`
- `src/api/ModelClient.h` — `StreamResponse` 签名无 override 形参
- `QueryLoop_多轮修正方案.md` 第 8.2 节明确指出"只有状态字段没有实际传参"

**目标行为**：`HandleMaxOutputTokens` 触发时，下次 `ModelClient::StreamResponse` 调用实际用升级后的 token 上限。

**改法**：

1. **`src/api/ModelClient.h`**：给 `StreamResponse` 增加 `int maxOutputTokensOverride = 0` 形参（默认 0 = 用模型默认值）。在函数内部，若该值 > 0，请求 JSON 的 `max_tokens` 字段用它。
2. **`src/core/QueryLoop.cpp` 的 `ApplyStepModelCall`**：调用 `StreamResponse` 时传入 `state.maxOutputTokensOverride`。
3. **常量核对**：`kEscalatedMaxTokens = 65536`（已存在），确保 `HandleMaxOutputTokens` 设 `state.maxOutputTokensOverride = kEscalatedMaxTokens`。

**前置任务**：无。

**验收标准**：
- [ ] mock ModelClient 记录请求体，断言 override 时 `max_tokens` 字段 == 65536
- [ ] 单元测试 `test_queryloop_max_output_tokens_escalate`：首次命中 max tokens → 升级 → 重试

---

## T05 — `fallback model` 内聚进主循环【P1】

**问题**：`QueryEngine::HandleFallback` 是循环外重试逻辑，导致模型切换与续轮状态割裂。fallback 应在 `ApplyStepModelCall` 内部按 `state.activeModel` 决策。

**证据**：
- `src/core/QueryEngine.h:156` — `HandleFallback` 存在但未接入 `RunTurn` 主路径
- `src/core/QueryLoop.h:49` — `state.activeModel` 字段已存在
- 三模型分工：`MAIN`（主推理）/ `VALIDATOR`（校验）/ `FALLBACK`（主模型失败时替代）

**目标行为**：`ApplyStepModelCall` 内：先 `state.activeModel = ctx.model`；若遇明确 fallback 触发条件（API 5xx/超时/限流），切 `state.activeModel = ctx.fallbackModel` 重试；fallback **只影响主模型调用，不影响 validator side query**。

**改法**：

1. `ApplyStepModelCall`（`QueryLoop.cpp:2521`）开头：`std::string activeModel = state.activeModel.empty() ? ctx.model : state.activeModel;`
2. 调用 `StreamResponse` 用 `activeModel`。
3. 捕获失败后，判定是否属 fallback 触发条件（5xx / 超时 / 限流 429），若是且 `!ctx.fallbackModel.empty()` 且 `activeModel != ctx.fallbackModel`：设 `state.activeModel = ctx.fallbackModel`，`continue`（回到 `ToolResultBudget` 重试），不丢 `transition reason`。
4. **禁止错误模式**：validator 不能跟着 fallback 走；fallback 后不能忘 `transition reason`（在续轮时显式携带）。

**前置任务**：T03（messagesForTurn 分离后再动模型切换更安全）。

**验收标准**：
- [ ] 单元测试：主模型 500 → 切 fallback → 续轮成功，validator 仍用原 validatorModel
- [ ] transcript 里能看到 `activeModel` 切换记录

---

## T06 — 统一续轮重组 helper `ContinueWithFollowup`【P1】

**问题**：validator/budget/stop-hook/max-token 等多处续轮各自 `ctx.messages.push_back + continue`，顺序无统一保证，状态散落。

**证据**：
- `src/core/QueryLoop.h:131` — `ContinueWithFollowup` 已声明（计划已落地，需核对调用点）
- `QueryLoop_多轮修正方案.md` 第 6.1 节定义该 helper 职责

**目标行为**：所有续轮强制走单一入口，固定顺序：

```
1. 本轮 assistantMessages 入历史
2. synthetic tool_result 入历史
3. followup meta message 入历史
4. 全部 persist + 写 transcript
5. 清空 turn-local 临时状态（assistantMessages/toolUseBlocks/toolResultMessages）
6. state.stage = QueryStage::ToolResultBudget
7. 重置必要恢复计数器
```

**改法**：

1. 核对 `ContinueWithFollowup` 实装（`QueryLoop.cpp` 内）含上述 7 步。
2. **全局搜索所有 `continue` 站点**（`grep -n "continue;" src/core/QueryLoop.cpp`），凡涉及注入 meta message 的，改调 `ContinueWithFollowup(ctx, state, followups, reason)`。
3. 删除散落的 `ctx.messages.push_back(...) ; ... continue;` 模式。

**前置任务**：T02、T03。

**验收标准**：
- [ ] `grep -n "ctx.messages.push_back" src/core/QueryLoop.cpp` 仅出现在 `ContinueWithFollowup` 及 `AppendTurnArtifacts` 内
- [ ] 所有续轮 transcript 顺序一致：assistant → tool_result → followup

---

## T07 — 引入 `ValidatorTier` 能力分层触发【P0，双模型修复第 1 步】

**问题**：cpp-agent 默认禁用校验，注释自认"同层模型净负"（`QueryLoop.cpp:685-701`）。一刀切禁用浪费了校验模型潜力。

**证据**：
- `src/core/QueryLoop.cpp:685-701` — `ShouldRunValidation` 注释明确写"Gemma 单模型优于 Gemma+Qwen 校验组合"
- `src/core/StateTypes.h:186` — `validatorEnabled = false` 默认关
- `src/core/LocalValidator.h` — 已有纯规则校验（`CheckUnixCommandsOnWindows`/`ForbidsFileOperations` 等），不调 LLM

**目标行为**：按校验模型相对主模型的能力分层：

```
ValidatorTier::Stronger  → 跑完整 LLM 校验（ApplyStepValidator）
ValidatorTier::Peer      → 只跑 LocalValidator 确定性规则，禁 LLM 校验
ValidatorTier::Weaker    → 完全禁用
ValidatorTier::Disabled  → 显式禁用
```

默认值从"空=禁用"改为"**自动探测=Peer**"。

**改法**：

1. **`src/core/StateTypes.h`** 新增枚举：
```cpp
// STRENGTHEN-07: validator capability tier relative to main model
enum class ValidatorTier { Stronger, Peer, Weaker, Disabled };
```

2. **新增 `ResolveValidatorTier` 函数**（`QueryLoop.cpp` 内，放在 `ResolveValidatorModel` 附近）。按模型族表判定：
```cpp
ValidatorTier ResolveValidatorTier(const std::string& mainModel,
                                   const std::string& validatorModel) {
  if (validatorModel.empty()) return ValidatorTier::Peer;  // 默认 Peer,不再 Disabled
  // 能力对照表(简化版,可扩展):
  //  - GPT-4o / Claude-Opus 校验 Gemma/Qwen-30B 以下 → Stronger
  //  - 同族同档(Gemma 校验 Gemma) → Peer
  //  - 弱模型校验强模型 → Weaker
  // 优先匹配显式环境变量 CPP_AGENT_VALIDATOR_TIER
  std::string t = ToLower(infra::GetEnvString("CPP_AGENT_VALIDATOR_TIER"));
  if (t == "stronger") return ValidatorTier::Stronger;
  if (t == "peer")     return ValidatorTier::Peer;
  if (t == "weaker")   return ValidatorTier::Weaker;
  if (t == "disabled") return ValidatorTier::Disabled;
  // 否则按模型族自动判定(实装一个 ModelFamilyComparator)
  return CompareModelFamilies(mainModel, validatorModel);
}
```

3. **改 `ShouldRunValidation`**：`Peer` 档返回 false（不跑 LLM 校验），只让 `ApplyStepValidator` 在 `Stronger` 时跑。`LocalValidator`（纯规则）在 `Peer` 及以上都跑。

4. **`AgentConfig`** 新增 `validatorTier` 字段，默认 `Peer`。

**前置任务**：无（这是双模型修复的入口）。

**验收标准**：
- [ ] 默认不设 validatorModel 时，tier=Peer，LocalValidator 跑、LLM 校验不跑
- [ ] 设 `CPP_AGENT_VALIDATOR_MODEL=gpt-4o` + 主模型 gemma 时，tier=Stronger，LLM 校验跑
- [ ] 单元测试覆盖 4 个 tier 的行为分支

---

## T08 — 废弃硬替换，改结构化 patch 注入【P0，双模型修复第 2 步】

**问题**：`ApplyTextCorrection`（`QueryLoop.cpp:2748-2749`）把校验模型文本硬替换主模型文本，导致主模型"人格/风格"断裂，输出前后不一致。

**证据**：
- `src/core/QueryLoop.cpp:2748-2749` — `if (!vresult.correctedText.empty()) ApplyTextCorrection(...)`
- local-ace `src/services/validation/index.ts:378-406` `applyTextCorrection` 同样硬替换 + 标 `isMeta`

**目标行为**：校验模型不直接改写，而是输出**结构化 patch**，主模型下一轮**自己决定**是否采纳。

**改法**：

1. **扩展 `ValidationResult`**（`src/core/QueryEngine.h:86`）增加 patch 字段：
```cpp
// STRENGTHEN-08: 结构化修正,取代整段 correctedText
struct ValidationPatch {
  std::string anchor;            // 锚点描述("第3段"/"关于X 的论断")
  std::string issue;             // 问题
  std::string suggestedReplace;  // 建议替换文本
};
std::vector<ValidationPatch> patches;
// 标记 deprecated,新代码不用:
// std::string correctedText;
```

2. **改 validator system prompt**（`BuildValidatorSystemPrompt`，`QueryLoop.cpp:1014`）：要求校验模型输出 `patches` 数组而非 `correctedText`。XML schema 改为：
```xml
<validation_json>
{ "patches": [{"anchor":"...","issue":"...","suggested_replace":"..."}],
  "tool_interventions":[...], "final_response_action":"approve|retry_from_tools",
  "retry_guidance":"..." }
</validation_json>
```

3. **`ParseValidationResponse`**（`QueryLoop.cpp:739`）解析 `patches` 数组。

4. **废弃 `ApplyTextCorrection`** 调用。改为把 patches 注入为一条 meta message：
```cpp
// STRENGTHEN-08: 不硬替换,把 patches 作为审稿意见注入下一轮
Message review;
review.role = MessageRole::User;
review.uuid = "validator-review";
review.isMeta = true;
std::string body = "[Validator review] Please consider these suggestions (apply at your own judgment):\n";
for (auto& p : vresult.patches) {
  body += "- Anchor: " + p.anchor + " | Issue: " + p.issue
        + " | Suggested: " + p.suggestedReplace + "\n";
}
review.content.push_back(ContentBlock::MakeText(body));
state.pendingFollowupMessages.push_back(review);
```

5. **去抖**：若主模型下一轮文本未采纳某 patch，记录该 patch 的 anchor 到 `state.rejectedPatchAnchors`，validator 后续不再就同 anchor 重复干预（需在 `BuildValidationContext` 里传 rejected anchors 给 validator，prompt 要求"勿重复已拒绝的 anchor"）。

**前置任务**：T07（先确定 tier，Stronger 档才走这条路）。

**验收标准**：
- [ ] transcript 里主模型文本**不再被静默改写**，而是看到 `[Validator review]` meta message
- [ ] 主模型下一轮若不采纳，不重复干预
- [ ] 单元测试：validator 输出 patch → 注入 meta → 主模型可读

---

## T09 — 动态有效率门控（自适应熔断）【P0，双模型修复第 3 步】

**问题**：现有熔断是固定阈值（`kMaxValidatorRetryContinuations=3`，`QueryLoop.cpp:3221`），无法感知"校验到底有没有用"。

**证据**：
- `src/core/QueryLoop.h:40` — `totalValidatorRetryCount`（session 级，从不重置）
- `src/core/QueryLoop.cpp:3221` — 固定 `kMaxValidatorRetryContinuations=3`
- local-ace `src/services/validation/index.ts` 无任何熔断

**目标行为**：滑动窗口统计校验干预有效率，自动升降 tier。

```
有效率 = (主模型采纳并改善的次数) / (校验干预总次数)
若有效率 < 0.3 → tier 自动 Stronger → Peer（降档）
若有效率 > 0.7 → 维持,可上调预算
```

**改法**：

1. **`QueryLoopInternalState`** 新增：
```cpp
// STRENGTHEN-09: 滑动窗口统计校验有效率
struct ValidatorOutcome {
  std::string anchor;       // 干预锚点
  bool accepted = false;    // 主模型是否采纳(下一轮文本检测)
  bool improved = false;    // 采纳后是否真的改善(可选,先只看 accepted)
};
std::deque<ValidatorOutcome> validatorOutcomes;  // 滑动窗口,size<=20
```

2. **判定"采纳"**：T08 注入 patch 后，下一轮 `ApplyStepModelCall` 之前，检查上一轮 assistant 文本是否含 suggestedReplace 的关键片段（或语义近似）。标记 `accepted`。

3. **新增 `ComputeValidatorEffectiveness`**：返回最近 20 次的 `accepted/total`。

4. **在 `ShouldRunValidation`（或 `ResolveValidatorTier` 调用点）**：每次进入 Validator stage 前，若 effectiveness < 0.3 且样本数 >= 10，自动把当前 tier 降一档（Stronger→Peer）。降档后记录日志 `[STRENGTHEN-09] validator tier auto-downgraded: effectiveness=0.2`。

5. **保留固定熔断作为硬上限**：T09 是软门控，`kMaxValidatorRetryContinuations` 仍是硬保护。

**前置任务**：T08（需要 patch 注入机制才能判定"采纳"）。

**验收标准**：
- [ ] 模拟 10 次校验全被主模型拒绝 → tier 自动降 Peer
- [ ] transcript 含降档日志
- [ ] 单元测试覆盖有效率计算

---

## T10 — 事前契约（execution_contract）【P1，双模型修复第 4 步】

**问题**：校验只在事后判断，主模型不知道会被怎么验收，错误反复发生。

**目标行为**：用户首条消息后，validator（仅 Stronger 档）生成一份 `execution_contract`（目标、约束、禁止动作、验收标准），注入主模型 system context。事后校验改为对照 contract 逐条验收。

**改法**：

1. **新增 `BuildContractRequest`**：用 SideQueryClient，输入用户首条消息，输出 JSON contract：
```json
{"goal":"...","constraints":["..."],"forbidden":["..."],
 "acceptance_criteria":["..."]}
```

2. **挂载点**：`QueryLoop` 首轮 `ModelCall` 之前，若 tier==Stronger，先 side-query 生成 contract，存入 `state.executionContract`，并在 `BuildMessagesForTurn` 时把 contract 文本注入 system 或首条 user meta。

3. **改 validator system prompt**：从"自由发挥"改为"对照 contract.acceptance_criteria 逐条 pass/fail，输出 `acceptance_results: [{criterion, pass, evidence}]`"。

4. **retry_from_tools 触发条件**：任一 criterion fail 才允许 retry，全 pass 则必须 approve。

**前置任务**：T07。

**验收标准**：
- [ ] Stronger 档首轮能看到 contract 注入
- [ ] validator 输出含 acceptance_results
- [ ] 全 pass 时强制 approve

---

## T11 — 并行预判校验（异步护栏）【P2】

**问题**：local-ace 为校验禁用流式执行（`query.ts:566-567`），cpp-agent 流式工具执行运行期关闭（`QueryLoop.cpp:2543 useStreamingExecution=false`）。校验与体验对立。

**目标行为**：主模型正常流式输出给用户；validator 异步 fire-and-forget；仅 validator 在主模型 turn 结束前返回 block 级干预，才对未 yield 的工具块施加 block；文本类干预转下轮 patch（T08）。

**改法**：

1. `ApplyStepModelCall` 开头，若 tier==Stronger，通过 `infra::ThreadPool::Global().Submit` 启动一个异步 validator 任务（输入：当前 messagesForTurn 快照），返回 future。
2. 流式输出照常 yield 给 TUI。
3. `ApplyStepModelCall` 结束前 `future.wait_for(短超时)`：若已返回 block 级 tool_interventions，对 `state.toolUseBlocks` 施加；超时则放弃本次校验。
4. **不要**为了校验禁用流式。

**前置任务**：T07、T08。

**验收标准**：
- [ ] 用户感知到流式输出不中断
- [ ] validator block 仍能在工具执行前生效

---

## T12 — BashClassifier 接通 LLM 分类【P1】

**问题**：`BashClassifier::Classify`（`permissions/BashClassifier.cpp:135`）有 `BuildClassifierPrompt`（行 113）但**从不调 LLM**，分类纯靠静态 pattern list。复杂命令无法判定。

**证据**：
- `src/permissions/BashClassifier.cpp:113` — `BuildClassifierPrompt` 存在
- `src/permissions/BashClassifier.cpp:135` — `Classify` 只查 `kReadOnlyCommands`/`kReadOnlyPrefixes`/`kDestructiveCommands`，无 LLM 调用

**目标行为**：pattern miss 时，调 SideQueryClient 做轻量分类，返回 read-only-allow / destructive-deny / default-deny。

**改法**：

1. `BashClassifier` 构造接收 `api::SideQueryClient*`（可选， nullptr 时退化为纯 pattern）。
2. `Classify` 流程：pattern 命中 → 直接返回；miss 且 sideQueryClient 非空 → 用 `BuildClassifierPrompt` 调 side query，解析结构化结果（强制 tool_choice 或 JSON 输出）。
3. 加超时（2s）和缓存（同命令同会话内复用结果）。

**前置任务**：无。

**验收标准**：
- [ ] 单元测试：pattern miss 的复杂命令经 mock LLM 返回正确分类
- [ ] 无 SideQueryClient 时退化为纯 pattern，行为不变

---

## T13 — 权限校验器按工具细分【P1】

**问题**：cpp-agent 权限校验粒度粗，缺 local-ace 那种按工具细分的校验器。

**证据**：
- local-ace `src/tools/BashTool/{modeValidation,pathValidation,readOnlyValidation,sedValidation}.ts` 各自独立
- cpp-agent `src/permissions/` 只有 `BashClassifier`/`PathValidator`/`PermissionEngine`/`PolicyLimits`

**改法**：

1. 在 `src/permissions/` 新增 `BashModeValidator.{h,cpp}`、`BashPathValidator.{h,cpp}`、`BashReadOnlyValidator.{h,cpp}`、`BashSedValidator.{h,cpp}`，分别移植 local-ace 对应文件的判定逻辑。
2. `BashClassifier::Classify` 改为组合调用这些细分校验器。
3. 每个校验器返回 `{allow, deny, reason}`，组合时 deny 优先。

**前置任务**：T12。

**验收标准**：
- [ ] 移植 local-ace 的 Bash 校验测试用例，全绿
- [ ] 危险命令（`rm -rf /`、`curl | sh`）被正确 deny

---

## T14 — 扩充工具集（移植 local-ace 工具）【P1】

**问题**：cpp-agent 仅 ~15 工具，local-ace ~50。缺常用工具影响能力边界。

**证据**：`src/tools/` 仅 Bash/FileRead/Write/Edit/Grep/Glob/Agent/TodoWrite/TaskCreate/TaskGet/Update/List/Stop/AskUserQuestion/NotebookEdit/Skill/ListMcpResources/ReadMcpResource/WebFetch/WebSearch。local-ace 多出 PowerShell/LSP/Monitor/Workflow/Cron/REPL/EnterPlanMode/ExitPlanMode/ToolSearch 等。

**改法**（按价值排序，分批移植）：
1. **PowerShellTool**（Windows 优先）：复用 `ProcessRunner`，移植 local-ace `src/tools/PowerShellTool/`。
2. **EnterPlanModeTool / ExitPlanModeTool**：纯状态切换工具，工作量小。
3. **ToolSearchTool**：cpp-agent 已有 `tools/ToolSearch.h`，需接通为可调用工具。
4. **SnipTool**：配合 T15 的 snip 能力。
5. 其余按需。

每个工具按 `ToolDef` + `ConcreteTool` 模式（`tools/Tool.h:224-297`）实装，并在 `ExecuteToolBlock` switch 加分支。

**前置任务**：T01（先通 MCP，工具生态才有意义）。

**验收标准**：
- [ ] 每移植一个工具：新增 `test_<tool>.cpp`，主模型能调用并拿到结果

---

## T15 — 独立 reactiveCompact + cachedMCConfig【P1】

**问题**：cpp-agent `compact/` 缺 reactiveCompact（413/media 错误触发的响应式压缩）独立实现，也缺 cachedMCConfig（缓存微压缩配置）。

**证据**：
- local-ace `src/services/compact/{reactiveCompact.ts,cachedMCConfig.ts}` 独立文件
- cpp-agent `src/compact/` 无对应文件（reactive 逻辑散在 `QueryLoop.cpp` 的 `DoReactiveCompact`，行 2201）

**改法**：

1. 新增 `src/compact/ReactiveCompact.{h,cpp}`：把 `DoReactiveCompact` 抽出成独立类，增加 media-size 错误的 strip-retry 路径（local-ace `reactiveCompact.ts` 的能力）。
2. 新增 `src/compact/CachedMCConfig.{h,cpp}`：移植 local-ace `cachedMCConfig.ts`，缓存微压缩配置避免每轮重算。
3. `QueryLoop` 改调这两个新类。

**前置任务**：T03。

**验收标准**：
- [ ] 413 错误触发 reactiveCompact，media 错误触发 strip-retry
- [ ] 微压缩配置被缓存，重复查询命中缓存

---

## T16 — 补 skillSearch / teamMemorySync【P2】

**问题**：cpp-agent 记忆子系统缺 skill 搜索和团队记忆同步。

**证据**：
- local-ace `src/services/{skillSearch,teamMemorySync}/` 独立子系统
- cpp-agent `src/memory/` 无对应

**改法**：
1. `src/memory/SkillSearch.{h,cpp}`：移植 skill 索引与检索。
2. `src/memory/TeamMemorySync.{h,cpp}`：团队记忆文件同步（多 agent 共享 MEMORY.md topic files）。

**前置任务**：无。

**验收标准**：
- [ ] skill 关键词能检索到匹配的 skill
- [ ] 多 agent 写同一 team memory 文件不冲突（文件锁）

---

## T17 — Hooks 事件广播系统 + 多执行器 + ssrfGuard【P2】

**问题**：cpp-agent hooks 仅基础执行器（`hooks/HookExecutor.{h,cpp}` + `HookTypes.h` 27 事件），缺事件广播、多执行后端、SSRF 防护。

**证据**：
- local-ace `src/utils/hooks/` 17 文件：`hookEvents.ts`（事件广播）、`execAgentHook.ts`/`execHttpHook.ts`/`execPromptHook.ts`（多执行器）、`ssrfGuard.ts`（SSRF 防护）、`AsyncHookRegistry.ts`
- cpp-agent `src/hooks/` 仅 3 文件

**改法**：
1. `src/hooks/HookEventBus.{h,cpp}`：移植 `hookEvents.ts` 的 `HookStartedEvent`/`HookProgressEvent`/`HookResponseEvent` 广播。
2. `src/hooks/{AgentHookExecutor,HttpHookExecutor,PromptHookExecutor}.{h,cpp}`：多执行后端。
3. `src/hooks/SsrfGuard.{h,cpp}`：URL 安全校验（私有 IP / localhost / 元数据服务地址阻断）。
4. `src/hooks/AsyncHookRegistry.{h,cpp}`：异步 hook 注册。

**前置任务**：无。

**验收标准**：
- [ ] hook 执行产生 started/progress/response 事件，SDK 消费方能订阅
- [ ] SSRF 危险 URL（169.254.169.254 等）被阻断

---

## T18 — 评估引入 OS 级沙箱【P2】

**问题**：cpp-agent `SandboxEnforcer` 是纯应用层 pattern 过滤，无 OS 级隔离。local-ace 桥接 `@anthropic-ai/sandbox-runtime` 包。

**证据**：
- `src/sandbox/SandboxEnforcer.cpp:67` — `CheckCommand` 纯 pattern
- local-ace `src/utils/sandbox/sandbox-adapter.ts` 桥接 runtime 包

**改法**（先评估再决定）：
1. 评估 `@anthropic-ai/sandbox-runtime` 是否有 C++ 等价物或可封装的 native lib。
2. Windows：评估 AppContainer / Job Object 限制。
3. Linux：评估 seccomp + namespaces（需 T00 POSIX 实装后）。
4. 若评估认为 pattern 过滤足够，本任务转为"加测试覆盖"。

**前置任务**：T00。

**验收标准**：
- [ ] 出具评估文档 `docs/SANDBOX_EVALUATION.md`，含选型理由
- [ ] 若引入 OS 沙箱：危险命令在隔离环境执行，无法逃逸

---

## T19 — VCR 录像回放【P2】

**问题**：缺 local-ace 的 VCR（录像回放）能力，难以离线复现 bug。

**证据**：local-ace `src/services/vcr.ts`。

**改法**：
1. `src/infra/VcrRecorder.{h,cpp}`：录制所有 ModelClient/SideQueryClient 的请求-响应对到 JSONL。
2. `src/infra/VcrReplayer.{h,cpp}`：回放模式劫持 model 调用，按匹配规则返回录制的响应。
3. 测试模式（`AGENT_VCR_MODE=record|replay|off`）切换。

**前置任务**：无。

**验收标准**：
- [ ] record 模式跑一遍真实会话产出 JSONL
- [ ] replay 模式无网络也能复现相同对话流

---

## T20 — 流式工具执行运行期开启【P2】

**问题**：`AGENT_FEATURE_STREAMING_TOOLS` 编译期开，但运行期 `useStreamingExecution = false`（`QueryLoop.cpp:2543`）。

**目标行为**：read-only 工具在模型仍流式输出时并发启动（参考 local-ace `StreamingToolExecutor` + T11）。

**改法**：
1. 增加 `AGENT_ENABLE_STREAMING_TOOLS` 环境变量运行期开关。
2. `ApplyStepModelCall` 的 SSE callback 里，收到完整 tool_use 块且工具 `IsReadOnly()==true` 时，提前提交到 ThreadPool 执行。
3. `ApplyStepRunTools` 收尾时 `getRemainingResults`。

**前置任务**：T11。

**验收标准**：
- [ ] 开启后，read-only 工具与模型流式并发，总延迟下降
- [ ] write 工具仍串行

---

## T21 — 同步 ARCHITECTURE_ANALYSIS.md parity 表【P2】

**问题**：`docs/ARCHITECTURE_ANALYSIS.md` 是 cpp-agent 自带的 parity 报告，但随修复推进会过时。

**改法**：每完成一个任务，更新该文档对应模块的 parity 状态（`✅ done` / `⚠️ partial` / `❌ missing`）。

**验收标准**：
- [ ] parity 表反映当前实装状态

---

## 执行顺序总览（依赖图）

```
T00 (POSIX 诚实性) ──────────────────────────┐
                                              │
T01 (MCP 调用) ──── T14 (工具扩充)            ├─→ T18 (OS 沙箱评估)
                                              │
T02 (续轮调度器) ─┬─ T03 (messagesForTurn) ─┬─ T05 (fallback 内聚)
                  │                          ├─ T15 (reactiveCompact)
                  └─ T06 (ContinueWithFollowup)
                                              │
T07 (ValidatorTier) ─┬─ T08 (patch 注入) ─┬─ T09 (动态门控)
                     │                     ├─ T10 (事前契约)
                     └─ T11 (并行预判) ──── T20 (流式工具)

T12 (BashClassifier LLM) ── T13 (权限细分)

T16 (skill/team memory)  T17 (hooks 广播)  T19 (VCR)  T21 (文档同步) — 独立
```

**建议分 4 批提交**：

| 批次 | 任务 | 目标 |
|---|---|---|
| 第 1 批 | T00, T01, T02, T04 | 修诚实性 + 最严重实装缺口 + QueryLoop 核心病灶 + 接口摆设 |
| 第 2 批 | T03, T05, T06, T07, T08, T09 | QueryLoop 状态分离 + 双模型协作重塑（核心质量） |
| 第 3 批 | T10, T11, T12, T13, T14, T15 | 双模型增强 + 权限 + 工具 + 压缩（生态质量） |
| 第 4 批 | T16, T17, T18, T19, T20, T21 | 生态扩展 + 沙箱 + 回放 + 文档（完整性） |

---

## 全局验收红线（任一未满足则该批次不能合并）

1. T01 后，MCP 工具能被真正调用（非 unknown tool）
2. T02 后，首轮只输出计划不再立即 completed，`turn_count > 1`
3. T04 后，`max_tokens=65536` 实际出现在请求体
4. T07 后，默认 Peer 档，LocalValidator 跑、LLM 校验不跑
5. T08 后，transcript 不再出现主模型文本被静默改写
6. T09 后，连续拒绝的 validator 自动降档
7. 所有新增/修改有对应单元测试
8. 真实工作目录回归：`<project>/.cpp-agent/session/transcript.jsonl` 含完整多轮链路（见修正方案文档第 7.3 节）

---

## 附录：双模型协作目标终态（T07-T11 全部完成后）

```
用户消息进入
   │
   ├─[Stronger 档] T10: validator 生成 execution_contract → 注入 system
   │
   ▼
主模型流式推理（正常 yield 给用户）
   │
   ├─[Stronger 档] T11: validator 异步并行启动（fire-and-forget）
   │
   ▼ 主模型 turn 结束
   │
   ├─ validator future.wait_for(短超时)
   │     ├─ 返回 block 级 tool_interventions → 对未执行工具施加 block
   │     ├─ 返回 patches → T08: 注入 [Validator review] meta（不硬替换）
   │     ├─ 返回 acceptance_results（T10 contract 验收）→ 任一 fail 才 retry
   │     └─ 超时 → 放弃本次校验
   │
   ├─ T09: 滑动窗口统计有效率
   │     └─ <0.3 → 自动 Stronger→Peer 降档
   │
   ▼
T02/T06: ContinueWithFollowup 统一重组续轮（固定顺序）
   │
   ▼
下一轮
```

**这才是"1+1>2"的协作语义**：校验模型从"串行事后审判者"变成"分层触发 + 异步并行 + 结构化审稿 + 自适应门控"的协作伙伴。主模型保持作者权和流式体验，校验模型在它真正更强时（Stronger 档）才介入，且介入方式是建议而非改写。

---

**文档版本**：v1.0
**生成依据**：cpp-agent 源码直接阅读 + local-ace 源码直接阅读 + `QueryLoop_多轮修正方案.md` + 两轮探索代理逐行分析
**维护约定**：每完成一个任务，对应章节标注 `[已完成 @ commit-hash]`，并更新 T21 parity 表
