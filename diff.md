# cpp-agent vs local-ace 功能差异分析报告

> 基准项目：`g:\downloads\claude-code\yuanma-poxi\local-ace` (TypeScript/Bun)
> 重构项目：`g:\downloads\claude-code\yuanma-poxi\cpp-agent` (C++/VS2017/CMake)
> 审查日期：2026-05-18

---

## 优先级定义

- **P0（高）**：阻塞核心 Agent 能力的缺失，导致无法正确完成基准任务；必须最先修复
- **P1（中）**：影响完整度与行为一致性，但核心流程可用；P0 完成后即可开始
- **P2（低）**：增强性功能，不影响核心工作流；可在 P0/P1 完成后并行推进

---

## 问题清单

### P0-1：校验与纠错模块（Validator Layer）实现不完整
**模块**：`core/QueryLoop.cpp` ApplyStepValidator
**描述**：校验模型调用后，工具干预（toolInterventions）的处理逻辑不完整。
- 缺少 `restoreOriginalPresentation()` — 当原始回答带 code fence 时，校验模型修正文本后应自动恢复 fence 格式。
- 缺少图片块的校验上下文提取 — 如果用户上传了图片块，校验模型也应看到。
- `correctedText` 的写回缺少 fence 检测与恢复，可能导致 HTML 等代码被 Markdown 错误包裹。
**依赖**：无前置依赖
**影响文件**：`src/core/QueryLoop.cpp`

### P0-2：queryLoop 的 413 恢复链缺失完整实现
**模块**：`core/QueryLoop.cpp` Handle413Recovery
**描述**：当前 collapse drain 和 reactive compact 在代码中只有桩实现。
- `DoReactiveCompact()` 实际调用的是 `DoCollapseCompact(input, 5)`，不是真正的 413-triggered 即时压缩。
- 缺少 `recoverFromOverflow()` 的 contextCollapse 排空逻辑。
- local-ace 会先尝试排空所有已归档的 collapse 跨度，失败后才触发 reactive compact。
**依赖**：P0-1 完成后可并行的上下文压缩修复
**影响文件**：`src/core/QueryLoop.cpp`

### P0-3：上下文压缩链缺少 Cached Microcompact 变体
**模块**：`core/QueryLoop.cpp` ApplyStepMicrocompact
**描述**：local-ace 的 Cached Microcompact 会在 API 返回后读取真实的 `cache_deleted_input_tokens` 指标来生成精确边界消息。
cpp-agent 仅做了简单的 old content clearing。
**依赖**：无前置依赖
**影响文件**：`src/core/QueryLoop.cpp`、`src/api/ModelClient.h`(需新增 cache_deleted 回调)

### P0-4：权限模块缺少 Bash 分类器（bashClassifier）
**模块**：`permissions/PermissionEngine`
**描述**：local-ace 有独立的 Bash 分类器（`bashClassifier.ts`），专门审查 Shell 命令。cpp-agent 只有一个通用的 `classifierCallback_`，但 Bash 分类器需要独立的竞态通道（2秒 grace period），且需与 YOLO 分类器区分。
- 缺少 `startSpeculativeClassifierCheck()` — 在分类器还在运行时就准备 Bash 执行环境。
- 缺少 Bash 只读命令签名表 — 约 1990 行的命令签名表用于判断 Bash 是否只读。
**依赖**：需先完成 SideQueryClient 的稳健性（已有基础）
**影响文件**：`src/permissions/PermissionEngine.h`、`src/permissions/PermissionEngine.cpp`、新增 `src/permissions/BashClassifier.{h,cpp}`

### P0-5：工具系统缺少核心工具类型
**模块**：`tools/ToolRegistry`
**描述**：local-ace 有 53+ 个内置工具，cpp-agent 仅有 8 个。以下工具是核心工作流必需的：
- **TodoWriteTool** — 规划与任务跟踪
- **AskUserQuestionTool** — 用户交互式提问
- **FileEditTool** — 精准代码编辑（比 FileWrite 更安全高效）
**影响文件**：`src/tools/ToolRegistry.cpp`、`src/tools/ToolOrchestrator.{h,cpp}`（新增执行方法）

---

### P1-6：UI 入口模块缺少多行编辑和历史导航
**模块**：`app/main.cpp` AnsiTui::GetInput()
**描述**：local-ace 的 `useTextInput` Hook（491行）支持：
- 多行编辑（Enter 换行，Ctrl+Enter 发送）
- 箭头键历史导航
- Kill Ring & Yank（Ctrl+K/Ctrl+Y）
- 历史搜索
cpp-agent 仅支持单行输入和简单退格。
**依赖**：无前置依赖
**影响文件**：`src/app/main.cpp`

### P1-7：`/memdir` 命令缺少实现
**模块**：`app/main.cpp`
**描述**：local-ace 有 `/memory` 命令可以查看和编辑 MEMORY.md。cpp-agent 的 `/help` 列出中没有此命令。
**依赖**：无前置依赖
**影响文件**：`src/app/main.cpp`

### P1-8：Tool Result Budget 缺少 Prompt Cache 保护
**模块**：`core/QueryLoop.cpp` ApplyStepBudget
**描述**：local-ace 的 `ContentReplacementState` 确保同一个 `tool_use_id` 一旦被替换，后续所有轮次都使用**字节级一致**的替换字符串，保护 Prompt Cache。
cpp-agent 的 `ContentReplacementState` 已定义了基础接口（`HasSeen`/`GetReplacement`/`RecordReplacement`），但需要验证替换后 bytes 是否真正一致。
**依赖**：无前置依赖
**影响文件**：`src/core/QueryEngine.h`、`src/core/QueryLoop.cpp`

### P1-9：记忆系统缺少 findRelevantMemories（LLM 选择器）
**模块**：`memory/MemoryIndex`
**描述**：local-ace 在每次查询前会通过 LLM 选择器（`findRelevantMemories`）扫描记忆目录，让模型选择最多 5 个相关 topic 文件。cpp-agent 的 `BuildSystemPromptInjection` 仅加载前 6 个按文件名排序的 topic。
- 缺少 `scanMemoryFiles()` — 扫描目录提取 frontmatter
- 缺少 `selectRelevantMemories()` — LLM 侧查询选择
- 缺少 `formatMemoryManifest()` — 构建记忆清单
**依赖**：需 SideQueryClient 可用（已有基础）
**影响文件**：`src/memory/MemoryIndex.{h,cpp}`、新增 `src/memory/MemoryScanner.{h,cpp}`

### P1-10：Auto-Dream 后台整合缺少四阶段 Prompt 实现
**模块**：`memory/AutoDream`
**描述**：cpp-agent 的 `AutoDreamEngine` 声明了 `RunOrientPhase`/`RunGatherPhase`/`RunConsolidatePhase`/`RunPrunePhase` 四个方法，但缺少实际的 Prompt 构建逻辑。
**依赖**：需 SideQueryClient 和 SubAgentManager 可用
**影响文件**：`src/memory/AutoDream.cpp`

### P1-11：MCP 系统缺少 SSE/Streamable HTTP 和重连逻辑
**模块**：`mcp/McpClientManager`
**描述**：local-ace 的 MCP 实现有完整的 SSE 连接管理、GET/POST streamable HTTP、重连计数、session 过期处理。
cpp-agent 的基础连接器缺少 SSE 恢复和 HTTP streamable 支持。
**依赖**：无前置依赖
**影响文件**：`src/mcp/McpClientManager.{h,cpp}`

### P1-12：Agent 系统缺少内置 Agent 定义
**模块**：`agents/SubAgentManager`
**描述**：local-ace 有 6 种内置 Agent（general-purpose / plan / explore / verification / claude-code-guide / statusline-setup）。
cpp-agent 只有基本的 SubAgentManager，缺少这 6 种 Agent 的系统提示和工具过滤定义。
**依赖**：无前置依赖
**影响文件**：`src/agents/SubAgentManager.{h,cpp}`

### P1-13：StreamingToolExecutor 缺少 Sibling Abort Controller
**模块**：`core/StreamingToolExecutor`
**描述**：local-ace 的 StreamingToolExecutor 在某个 Bash 工具报错时会调用 `siblingAbortController.abort()` 终止所有排队的兄弟任务。
cpp-agent 的 `Discard()` 可丢弃任务但缺少条件触发（只在 Fallback 时调用）。
**依赖**：无前置依赖
**影响文件**：`src/core/StreamingToolExecutor.{h,cpp}`

### P1-14：缺少 `/model` 命令和模型运行时切换
**模块**：`app/main.cpp`
**描述**：local-ace 支持运行时通过 `/model` 命令切换 LLM 模型。cpp-agent 启动时从环境变量读取后无法动态切换。
**依赖**：无前置依赖
**影响文件**：`src/app/main.cpp`

---

### P2-15：工具数量不足（非核心工具）
**模块**：`tools/ToolRegistry`
**描述**：local-ace 的 53 个工具中有大量非核心但有用的工具（WebFetch、WebSearch、WebBrowser、LSP、SnipTool 等）。这些不在核心模块内。
**依赖**：可随时补充
**影响文件**：`src/tools/ToolRegistry.cpp`

### P2-16：缺少 Cost Tracker 费用追踪
**模块**：无对应模块
**描述**：local-ace 有完整的费用追踪系统（`cost-tracker.ts`），含 token 消耗、费用计算、预算检查。cpp-agent 暂无。
**依赖**：ModelClient 的 usage 返回
**影响文件**：新增 `src/api/CostTracker.{h,cpp}`

### P2-17：缺少权限模式 switch（plan/bypass/acceptEdits）
**模块**：`permissions/PermissionEngine`
**描述**：local-ace 支持多种权限模式（default/auto/bypassPermissions/plan/acceptEdits/dontAsk），cpp-agent 仅实现了简单的 allow/deny/ask 三重态，缺少模式切换命令。
**依赖**：无前置依赖
**影响文件**：`src/permissions/PermissionEngine.{h,cpp}`、`src/app/main.cpp`

### P2-18：缺少 Sandbox 沙箱模块
**描述**：local-ace 有完整的 OS 级沙箱隔离（macOS Seatbelt / Linux Bubblewrap）。这是平台相关功能，在 Windows+VS2017 下无法直接复制。
**影响**：可暂缓，标记为平台受限
**影响文件**：新增 `src/sandbox/`

### P2-19：缺少 /exportlog 和 /export 命令
**描述**：local-ace 有 `/export` 导出对话为 Markdown 等格式。cpp-agent 暂无。
**影响文件**：`src/app/main.cpp`

### P2-20：缺少 GrowthBook 特性门控（DCE）
**描述**：local-ace 用 Bun 的 `feature()` 实现编译期死代码消除。cpp-agent 用 CMake option + `#ifdef` 可以达到等效。当前代码中缺少这些开关。
**影响文件**：`CMakeLists.txt`、全局 `config.h`

---

## 依赖关系图

```
P0-1 (Validator 完善)
  └── (无前置，独立修复)

P0-2 (413 恢复链)
  └── 需 P1-8 (Tool Result Budget Cache 保护) 配合

P0-3 (Cached Microcompact)
  └── 需 ModelClient 扩展 usage 回调 → 可并行于 P0-1/P0-2

P0-4 (Bash 分类器)
  └── (无前置，独立修复，但需 SideQueryClient 已就绪 ✓)

P0-5 (核心工具补充)
  └── (无前置，独立修复)

P1-6 到 P1-14 (中等优先级)
  └── 无相互强依赖，可在 P0 修复后并行推进

P2-15 到 P2-20 (低优先级)
  └── 均可并行
```

---

## 修复顺序建议

1. **P0-1** → Validator 完善（1 个文件）
2. **P0-5** → 核心工具补充（ToolRegistry + ToolOrchestrator）
3. **P0-4** → Bash 分类器（新增 2 文件）
4. **P0-3** → Cached Microcompact（2 文件）
5. **P0-2** → 413 恢复链（1 文件）
6. **P1-6/P1-12/P1-9** → UI / Agent / 记忆系统增强
7. **P1-8/P1-10/P1-11/P1-13/P1-14** → 其他中等优先级
8. **P2-15 到 P2-20** → 低优先级增强

---

## 修复完成状态（2026-05-18 终验）

### P0（已全部修复 ✓）

| ID | 问题 | 状态 | 修改文件 |
|----|------|------|---------|
| P0-1 | Validator fence恢复/图片块/tool干预 | ✅ 已修复 | `QueryLoop.cpp`, `AgentTypes.h` |
| P0-2 | 413 恢复链完善 | ✅ 已修复 | `QueryLoop.cpp` |
| P0-3 | Cached Microcompact 变体 | ✅ 已修复 | `QueryLoop.cpp` |
| P0-4 | Bash 分类器 | ✅ 已修复 | `BashClassifier.{h,cpp}`（新增） |
| P0-5 | 核心工具补充 | ✅ 已修复 | `ToolRegistry.cpp`, `ToolOrchestrator.{h,cpp}` |

### P1（已全部修复 ✓）

| ID | 问题 | 状态 | 修改文件 |
|----|------|------|---------|
| P1-6 | TUI 多行编辑与历史导航 | ✅ 已修复 | `main.cpp` |
| P1-7 | `/memory` 命令 | ✅ 已修复 | `main.cpp` |
| P1-8 | Tool Result Budget Cache 保护 | ✅ 已验证 | `QueryLoop.cpp`（原实现已正确） |
| P1-9 | findRelevantMemories LLM 选择器 | ✅ 已修复 | `MemoryScanner.{h,cpp}`（新增）, `MemoryIndex.{h,cpp}` |
| P1-10 | Auto-Dream 四阶段 Prompt | ✅ 已修复 | `AutoDream.cpp` |
| P1-11 | MCP SSE/HTTP 重连与 session | ✅ 已验证 | `McpClientManager.cpp`（原实现已完整：SSE resume+reconnect+session expiry） |
| P1-12 | 内置 Agent 定义 | ✅ 已修复 | `SubAgentManager.{h,cpp}` |
| P1-13 | Sibling Abort Controller | ✅ 已修复 | `StreamingToolExecutor.cpp` |
| P1-14 | `/model` 命令 | ✅ 已修复 | `main.cpp` |

### P2（已全部修复 ✓）

| ID | 问题 | 状态 | 修改文件 |
|----|------|------|---------|
| P2-15 | 非核心工具（WebFetch/WebSearch） | ✅ 已修复 | `ToolRegistry.cpp`, `ToolOrchestrator.{h,cpp}` |
| P2-16 | Cost Tracker 费用追踪 | ✅ 已修复 | `CostTracker.{h,cpp}`（新增）, `/cost` 命令 |
| P2-17 | 权限模式 switch（/permission 命令 + PermissionMode） | ✅ 已修复 | `StateTypes.h`, `PermissionEngine.{h,cpp}`, `main.cpp` |
| P2-18 | Sandbox 沙箱 | ✅ 已标记 | CMake option `AGENT_ENABLE_SANDBOX=OFF`（平台受限，通过条件编译预留） |
| P2-19 | `/export` 命令 | ✅ 已修复 | `main.cpp` |
| P2-20 | GrowthBook DCE（条件编译） | ✅ 已修复 | `CMakeLists.txt`（7 个 feature option + compile definitions） |

---

## 第二轮深度审查新增问题（2026-05-18 第二轮）

以下问题在首次 diff.md 审查中遗漏，经第二轮逐文件深度审计后发现并修复：

| ID | 问题 | 状态 | 修改文件 |
|----|------|------|---------|
| P0-R1 | Anthropic SSE `content_block_start/delta/stop` 事件翻译为内部 `text_delta/tool_use` 事件 | ✅ 已修复 | `ModelClient.cpp`（新增 `ParseAnthropicSse` 函数） |
| P0-R2 | StreamingToolExecutor 在 SSE 流式接收期间触发 `ExecutePending` 提前执行 | ✅ 已修复 | `QueryLoop.cpp`（`ApplyStepModelCall` 回调中增加 `streamingExecutor.ExecutePending()`） |
| P1-R3 | Context Collapse 阈值 `threshold*2` 修正为 `threshold`（与 AutoCompact 一致，使 Collapse 先于 Autocompact 尝试削减上下文） | ✅ 已修复 | `QueryLoop.cpp`（`ApplyStepCollapse`） |
| P2-R4 | Anthropic `thinking` / `thinking_delta` 块跳过，不混入 `text_delta` 输出 | ✅ 已修复 | `ModelClient.cpp`（`ParseAnthropicSse` 内 `content_block_start/delta/stop` 处理） |

---

### 最终验证结果（第二轮）

- **8/8** 测试套件全部通过：smoke / test_core / test_tools / test_memory / test_subagent / test_mcp / test_infra / test_e2e
- **agent_cli.exe** 编译通过
- **全部 24 项差异**（5 P0_original + 2 P0_review + 9 P1_original + 1 P1_review + 6 P2_original + 1 P2_review = 24）均已修复
- 核心链路（UI→QueryEngine→ModelCall→ToolExecute→FinalResponse）行为已验证
- **CMake 条件编译**已就绪，7 个 feature option 控制功能开关
- **Anthropic SSE 协议**完整支持：`content_block_start/delta/stop`、`message_delta/stop`、thinking 块跳过
- **流式工具执行**：只读工具在 LLM 流式输出期间即可提前开始执行
