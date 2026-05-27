# local-ace vs cpp-agent 完整架构分析报告

> 基准项目: `local-ace` (TypeScript/Bun, ~500K行, 2000+文件)
> 重构项目: `cpp-agent` (C++17/VS2017/CMake, 22源文件)
> 分析日期: 2026-05-25

---

## 1. 系统架构图

### 1.1 local-ace 六层架构

```mermaid
graph TB
    subgraph Entry["Entry Layer"]
        CLI["bootstrap-entry.ts → cli.tsx → main.tsx"]
        Fast["Fast-path: --version, --daemon-worker"]
    end

    subgraph Present["Presentation Layer"]
        Components["components/ 146+ dirs"]
        Ink["ink/ TUI framework"]
        Hooks["hooks/ 80+ files"]
        Vim["vim/ 5 files"]
    end

    subgraph Management["Management Layer"]
        Perms["utils/permissions/ 27 files"]
        State["state/ 6 files"]
        Services["services/ 20+ subdirs"]
        Settings["settings/ 17 files"]
        Plugins["plugins/ 45 files"]
    end

    subgraph Core["Core Engine Layer"]
        QE["QueryEngine.ts 1295行"]
        QL["query.ts 2362行"]
        Loop["QueryLoop: Budget→Snip→Micro→Collapse→Auto→ModelCall→Validator→StopHooks→RunTools→Terminate"]
    end

    subgraph Tool["Tool System"]
        Tools["53+ 工具目录"]
        Bash["BashTool, FileRead, FileWrite, FileEdit"]
        AgentTool["AgentTool: 6 内置Agent"]
        WebTools["WebFetch, WebSearch, WebBrowser"]
        TaskTools["TaskCreate/Get/Update/List/Stop"]
    end

    subgraph Infra["Infrastructure Layer"]
        MCP["MCP Integration 20+ files"]
        Memory["Memory System: MEMORY.md + topic files"]
        AutoDream["Auto-Dream 后台记忆整合"]
        HookSystem["Hook System: 24 event types"]
        Session["Session & State Persistence"]
    end

    Entry --> Present
    Entry --> Core
    Present --> Management
    Core --> Tool
    Core --> Infra
    Management --> Infra
```

### 1.2 cpp-agent 对应架构

```mermaid
graph TB
    subgraph Entry["Entry: app/main.cpp"]
        REPL["Interactive REPL CLI"]
        Cmds["/help, /model, /memory, /tools, /export, /copy..."]
    end

    subgraph Core["Core Engine"]
        QE["QueryEngine: SubmitUserPrompt, RunTurn"]
        QL["QueryLoop: 10-step state machine"]
        ST["StreamingToolExecutor"]
        AT["AgentTypes.h: Message, ContentBlock, Usage"]
        ST2["StateTypes.h: SessionMetadata, TodoList"]
    end

    subgraph ToolSystem["Tool System"]
        TR["ToolRegistry: RegisterTool, ListTools"]
        TO["ToolOrchestrator: Partition, Execute"]
        Tools["22 tool methods: Bash, FileRead, FileWrite, etc."]
    end

    subgraph Hooks["Hooks System"]
        HT["HookTypes: 24 event types"]
        HC["HookConfig: Load, Match"]
        HE["HookExecutor: Execute, PreToolUse, PostToolUse, Stop..."]
    end

    subgraph Infra["Infrastructure"]
        MCP["McpClientManager: Connect, Tools, Prompts, Resources"]
        Memory["MemoryIndex: MEMORY.md, topic files"]
        AD["AutoDream: 5-Gate, 4-Phase"]
        MS["MemoryScanner: Scan, Select"]
        SM["SessionManager: Snapshot, Transcript, ModelIO"]
        PR["ProcessRunner: CreateProcess, async I/O"]
        SW["StabilityWatchdog: Health monitoring"]
        PE["PermissionEngine: Allow/Deny/Ask"]
        BC["BashClassifier: Safety classification"]
    end

    subgraph API["API Layer"]
        MC["ModelClient: HttpLlmClient, SSE streaming"]
        SQ["SideQueryClient: Classification, Validation"]
        CT["CostTracker: Token usage tracking"]
    end

    subgraph Agents["SubAgent System"]
        SAM["SubAgentManager: spawn, fork, lifecycle"]
        SAW["SubAgentWorkerProtocol: IPC protocol"]
    end

    Entry --> Core
    Core --> ToolSystem
    Core --> Hooks
    Core --> Infra
    Core --> API
    Core --> Agents
    ToolSystem --> Infra
    Hooks --> Infra
```

---

## 2. 用例图

```mermaid
graph LR
    User["👤 用户"] -->|输入prompt| QE["QueryEngine"]
    User -->|CLI命令| CLI["/model /memory /tools..."]
    User -->|配置| Config["AgentConfig"]
    
    QE -->|组装system prompt| SP["SystemPrompt\n(CLAUDE.md + Memory + MCP)"]
    QE -->|驱动状态机| Loop["QueryLoop 10-step"]
    
    Loop -->|调用LLM| API["ModelClient\n(Anthropic/OpenAI API)"]
    Loop -->|执行工具| TO["ToolOrchestrator"]
    Loop -->|校验结果| VAL["Validator\n(SideQueryClient)"]
    Loop -->|权限检查| PE["PermissionEngine"]
    
    TO -->|Shell命令| Bash["BashTool"]
    TO -->|文件操作| File["FileRead/Write/Edit"]
    TO -->|搜索| Grep["GrepTool/GlobTool"]
    TO -->|子代理| Agent["AgentTool (SubAgentManager)"]
    TO -->|任务管理| Task["TaskCreate/Get/Update/List/Stop"]
    TO -->|MCP工具| MCPTool["MCP工具 (远程)"]
    
    SubAgent["SubAgentManager"] -->|Fork| Fork["buildForkedMessages"]
    SubAgent -->|后台| Dream["AutoDream"]
    
    Memory["MemoryIndex"] -->|读取| MEM["MEMORY.md"]
    Memory -->|搜索| Scanner["MemoryScanner\n+ SideQuery"]
    Memory -->|写入| Write["UpsertPointer + WriteTopic"]
    
    Hook["HookExecutor"] -->|事件| Events["24 event types"]
    Hook -->|执行| Exec["Command/Prompt/Agent/HTTP hooks"]
```

---

## 3. 数据流图

### 3.1 主查询数据流

```mermaid
flowchart TD
    A["用户输入 Prompt"] --> B["QueryEngine.SubmitUserPrompt()"]
    B --> C["BuildEffectiveSystemPrompt()\n(CLAUDE.md + MEMORY.md + MCP tools)"]
    C --> D["添加到 messages[]"]
    D --> E["QueryLoop.RunFull()"]
    
    E --> F{"QueryStage?"}
    F -->|ToolResultBudget| G["截断过大工具结果"]
    F -->|Snip| H["裁剪历史消息"]
    F -->|Microcompact| I["替换过期大结果"]
    F -->|Collapse| J["折叠长对话"]
    F -->|Autocompact| K["全局摘要压缩"]
    F -->|ModelCall| L["调用 LLM API\n(SSE 流式响应)"]
    F -->|Validator| M["校验模型输出\n(SideQueryClient)"]
    F -->|StopHooks| N["执行 Stop hooks"]
    F -->|RunTools| O["并发/串行执行工具"]
    
    L -->|text_delta| P["累积文本"]
    L -->|tool_use| Q["收集工具调用块"]
    L -->|stop_reason| R{"停止原因?"}
    R -->|end_turn| S["完成"]
    R -->|tool_use| O
    
    O -->|只读工具| T1["并发Batch\n(最大10)"]
    O -->|写工具| T2["串行Batch"]
    T1 --> U["收集tool_result"]
    T2 --> U
    U --> V["注入到messages[]"]
    V --> E
    
    M -->|通过| O
    M -->|拦截| W["文本纠正/工具重写"]
    W --> E
    
    S --> X["返回结果给用户"]
```

### 3.2 MCP 数据流

```mermaid
flowchart LR
    Config["MCP Server 配置"] --> Register["McpClientManager.RegisterServer()"]
    Register --> Connect["ConnectServer()\n(stdio/SSE/StreamableHTTP)"]
    Connect --> Init["Initialize (JSON-RPC)"]
    Init --> Cap["获取 capabilities"]
    Cap --> Tools["RefreshTools: tools/list"]
    Cap --> Prompts["RefreshPrompts: prompts/list"]
    Cap --> Resources["RefreshResources: resources/list"]
    
    Tools --> Map["映射工具名:\nmcp__{server}__{tool}"]
    Map --> Registry["注入 ToolRegistry"]
    
    Agent["Agent 调用工具"] --> Dispatch["ToolOrchestrator 识别 mcp__ 前缀"]
    Dispatch --> Call["McpTransport.Send()\n(tools/call JSON-RPC)"]
    Call --> Result["返回 tool_result"]
```

### 3.3 Memory 数据流

```mermaid
flowchart TD
    Start["Agent 启动"] --> Load["MemoryIndex.ReadEntrypoint()\n读取 MEMORY.md"]
    Load --> Trunc["双阈值截断:\n≤200行, ≤25KB"]
    Trunc --> Parse["ParsePointers()\n提取索引条目"]
    Parse --> Inject["BuildSystemPromptInjection()\n注入 system prompt"]
    
    User["用户查询"] --> Find["FindRelevantMemories()"]
    Find --> Scan["MemoryScanner.ScanMemoryFiles()\n收集 topic/*.md frontmatter"]
    Scan --> Select["SideQueryClient.Query()\nLLM 选择最相关文件 ≤5"]
    Select --> Read["LoadTopicFiles()\n读取选中文件"]
    Read --> Inject2["注入 system prompt"]
    
    Agent["Agent 写入"] --> Upsert["UpsertPointer()\n更新 MEMORY.md 索引"]
    Agent --> Write["WriteTopicFile()\n写入 topic/*.md"]
    
    Idle["空闲时"] --> Gate["AutoDream 5-Gate:\nEnable/Time/Scan/Session/Lock"]
    Gate --> Dream["4-Phase Prompt:\nOrient→Gather→Consolidate→Prune"]
    Dream --> Update["更新 MEMORY.md + topic files"]
```

### 3.4 Hooks 数据流

```mermaid
flowchart TD
    Event["触发事件\n(24 types)"] --> Config["HookConfig.GetMatchingHooks()\n按 event+matcher 过滤"]
    Config --> Execute["HookExecutor.RunHooksForEvent()"]
    
    Execute --> Type{"Hook类型?"}
    Type -->|Command| Cmd["ExecuteCommandHook()\nProcessRunner 执行 shell"]
    Type -->|Prompt| Pr["execPromptHook()\n发送给 LLM 处理"]
    Type -->|Agent| Ag["execAgentHook()\n启动子 Agent 处理"]
    Type -->|HTTP| Http["execHttpHook()\nHTTP POST 远程"]
    
    Cmd --> Parse["ParseHookOutput()\n解析 JSON stdout"]
    Pr --> Parse
    Ag --> Parse
    Http --> Parse
    
    Parse --> Decision{"决策?"}
    Decision -->|allow| Allow["允许操作继续"]
    Decision -->|deny| Deny["阻止操作"]
    Decision -->|ask| Ask["请求用户确认"]
    Decision -->|message| Inject["注入消息到对话"]
```

---

## 4. 模块逻辑结构关系

### 4.1 local-ace 模块依赖图

```mermaid
graph TB
    main["main.tsx (入口: 5800行)"]
    
    main --> QE["QueryEngine.ts\n组装系统prompt\n管理会话"]
    main --> Cmds["commands.ts\n注册80+命令"]
    main --> Tools["tools.ts\n注册53+工具"]
    
    QE --> Query["query.ts (2362行)\n核心查询循环"]
    
    Query --> Compact["services/compact/\nsnip/micro/collapse/auto"]
    Query --> API["services/api/\nclaude.ts/errors.ts/retry.ts"]
    Query --> ToolExec["services/tools/\nStreamingToolExecutor"]
    Query --> Valid["services/validation/\n校验器模型"]
    Query --> Hooks["hooks.ts (5000+行)\nHook执行引擎"]
    
    Tools --> AgentT["AgentTool/\n6内置Agent"]
    Tools --> BashT["BashTool"]
    Tools --> FileT["FileRead/Write/Edit"]
    Tools --> GrepT["GrepTool"]
    Tools --> TaskT["TaskCreate/Get/Update/List/Stop"]
    Tools --> WebT["WebFetch/Search"]
    
    Hooks --> HookUtils["utils/hooks/\nAsyncHookRegistry\nhookEvents.ts\nsessionHooks.ts"]
    Hooks --> HookExec["execCommandHook\nexecPromptHook\nexecAgentHook\nexecHttpHook"]
    
    API --> ModelC["ModelClient\nSSE streaming\nAnthropic API"]
    
    MCP["services/mcp/\n20+ files"] --> MCPClient["client.ts\nMCPConnectionManager.tsx"]
    MCP --> MCPAuth["auth.ts\nOAuth flow"]
    
    Memory["memdir/ 9 files"] --> MemIdx["memdir.ts: MEMORY.md"]
    Memory --> MemScan["memoryScan.ts\nfindRelevantMemories"]
    Memory --> AutoDream["services/autoDream/\nAuto-Dream engine"]
    
    State["state/AppState.ts"] --> Session["Session持久化"]
    Perms["utils/permissions/ 27 files"] --> PE["PermissionEngine"]
```

### 4.2 cpp-agent 模块依赖图

```mermaid
graph TB
    main["app/main.cpp\nREPL CLI入口"]
    
    main --> QE["core/QueryEngine\nSubmitUserPrompt/RunTurn"]
    main --> TR["tools/ToolRegistry\nRegisterTool/ListTools"]
    main --> SM["infra/SessionManager\nSnapshot/Transcript"]
    main --> MI["memory/MemoryIndex\nMEMORY.md管理"]
    main --> SW["infra/StabilityWatchdog\nHealth monitor"]
    
    QE --> QL["core/QueryLoop\n10-step state machine"]
    QE --> MC["api/ModelClient\nHttpLlmClient/SSE"]
    QE --> SQ["api/SideQueryClient\nClassification"]
    
    QL --> TO["tools/ToolOrchestrator\nPartition/Execute"]
    QL --> PE["permissions/PermissionEngine\nAllow/Deny/Ask"]
    QL --> HE["hooks/HookExecutor\n24 event types"]
    QL --> CT["api/CostTracker\nToken counting"]
    
    TO --> PR["infra/ProcessRunner\nSubprocess execution"]
    TO --> SAM["agents/SubAgentManager\nFork/Background"]
    TO --> MCM["mcp/McpClientManager\nServer lifecycle"]
    
    MI --> MS["memory/MemoryScanner\nScan/Select"]
    MI --> AD["memory/AutoDream\n5-Gate/4-Phase"]
    
    HE --> HC["hooks/HookConfig\nLoad/Match"]
    HE --> HT["hooks/HookTypes\n24 event types + schemas"]
    
    BC["permissions/BashClassifier"] --> PE
    ST["core/StreamingToolExecutor"] --> TO
    SAW["agents/SubAgentWorkerProtocol"] --> SAM
    PL["infra/ProtoLite"] --> SM
```

---

## 5. PDF 文档准确性验证

### 验证结论表

| PDF 章节 | 描述内容 | local-ace 实际代码 | cpp-agent 对应 | 准确性 |
|----------|---------|-------------------|---------------|--------|
| 1-项目整体架构 | 六层架构: Entry/Presentation/Management/Core/Tool/Infra | 完全匹配 (entrypoints/, components/, utils/services/, QueryEngine+query.ts, tools/, services/) | 对应实现 | ✅ 准确 |
| 2-核心业务逻辑 | QueryLoop 10步状态机 | Budget→Snip→Micro→Collapse→Auto→ModelCall→Validator→StopHooks→RunTools→Terminate | 完全对应 | ✅ 准确 |
| 3-大模型交互 | SSE流式响应, Anthropic API, SideQuery | ModelClient + SideQueryClient | HttpLlmClient + SideQueryClient | ✅ 准确 |
| 4.1-工具系统 | 53+工具, 并发/串行编排, MCP扩展 | ToolOrchestrator.PartitionToolCalls(), StreamingToolExecutor | ToolOrchestrator.PartitionToolCalls() | ✅ 准确 |
| 4.2-记忆系统 | MEMORY.md双阈值截断, topic文件, AutoDream | MemoryIndex.TruncateEntrypointContent, AutoDream 5-gate | MemoryIndex.TruncateEntrypointContent, AutoDreamEngine | ✅ 准确 |
| 4.3-Agent系统 | 6内置Agent, Fork机制, 后台/前台 | AgentTool/built-in/, SubAgentManager | SubAgentManager.BuildForkedMessages | ✅ 准确 |
| 5-代码规范 | Dead code elimination, feature flags | `feature()` macro, `process.env.USER_TYPE===ant` | CMake options: AGENT_ENABLE_* | ✅ 准确 |
| 6-配置管理 | settings.json, managed paths, MDM | utils/settings/ 17 files | (cpp-agent: AgentConfig硬编码) | ⚠️ 部分 |
| 7-数据流转 | User→QueryEngine→QueryLoop→Tools→Results | 完全匹配 | 完全对应 | ✅ 准确 |

**总结**: PDF文档描述与 local-ace 实际代码高度一致，所有核心架构描述均准确。6个PDF（9个章节）覆盖了项目的完整架构。

---

## 6. 模块对照表 (local-ace → cpp-agent)

| # | local-ace 模块 | 文件 | cpp-agent 对应 | 文件 | 状态 |
|---|---------------|------|---------------|------|------|
| 1 | Entrypoint/CLI | main.tsx, cli.tsx | app/main.cpp | main.cpp | ✅ 完成 |
| 2 | QueryEngine | QueryEngine.ts | core/QueryEngine | QueryEngine.h/cpp | ✅ 完成 |
| 3 | QueryLoop | query.ts | core/QueryLoop | QueryLoop.h/cpp | ✅ 完成 |
| 4 | Validation | services/validation/ | (SideQuery) | QueryLoop validator step | ✅ 完成 |
| 5 | Tool System | tools.ts, Tool.ts, 53 dirs | tools/ToolRegistry+Orchestrator | .h/.cpp | ✅ 基本完成 |
| 6 | Bash/Shell | BashTool/ | ToolOrchestrator::ExecuteBash | cpp | ✅ 完成 |
| 7 | FileRead/Write/Edit | FileReadTool/等 | ToolOrchestrator::ExecuteFileRead等 | cpp | ✅ 完成 |
| 8 | Grep/Glob | GrepTool/, GlobTool/ | ToolOrchestrator::ExecuteGrep/Glob | cpp | ✅ 完成 |
| 9 | Task Tools | TaskCreateTool/等5个 | ToolOrchestrator::ExecuteTask* | cpp | ⚠️ 部分stub |
| 10 | Skill Tool | SkillTool/ | ToolOrchestrator::ExecuteSkill | cpp | ⚠️ 简化版 |
| 11 | Plan Mode | EnterPlanModeTool/ | ToolOrchestrator::ExecuteEnterPlanMode | cpp | ✅ 完成 |
| 12 | MCP Tools | MCPTool/, ListMcpResourcesTool/ | McpClientManager + ToolOrch | cpp | ⚠️ ListMcpResources stub |
| 13 | WebFetch/Search | WebFetchTool/, WebSearchTool/ | ToolOrchestrator::ExecuteWebFetch/Search | cpp | ⚠️ 简化版 |
| 14 | NotebookEdit | NotebookEditTool/ | ToolOrchestrator::ExecuteNotebookEdit | cpp | ❌ stub |
| 15 | Permission | utils/permissions/ 27 files | permissions/PermissionEngine+BashClassifier | .h/.cpp | ✅ 完成 |
| 16 | MCP Integration | services/mcp/ 20+ files | mcp/McpClientManager | .h/.cpp | ✅ 完成 |
| 17 | Memory System | memdir/ 9 files + services/autoDream/ | memory/MemoryIndex+Scanner+AutoDream | .h/.cpp | ✅ 完成 |
| 18 | Hook System | hooks.ts (5000+行) + utils/hooks/ 18 files | hooks/HookExecutor+Config+Types | .h/.cpp | ✅ 完成 |
| 19 | Session Management | utils/sessionStorage.ts | infra/SessionManager | .h/.cpp | ✅ 完成 |
| 20 | Process Runner | (系统调用) | infra/ProcessRunner | .h/.cpp | ✅ 完成 |
| 21 | Stability Watchdog | (内建) | infra/StabilityWatchdog | .h/.cpp | ✅ 完成 |
| 22 | SubAgent System | AgentTool/ + swarm/ | agents/SubAgentManager+Worker | .h/.cpp | ✅ 完成 |
| 23 | Model Client | services/api/claude.ts | api/ModelClient+HttpLlmClient | .h/.cpp | ✅ 完成 |
| 24 | Cost Tracker | cost-tracker.ts | api/CostTracker | .h/.cpp | ✅ 完成 |
| 25 | Terminal UI | components/ + ink/ | (ANSI CLI in main.cpp) | main.cpp | ✅ 功能等效 |

### 图例
- ✅ 完成: 功能完整实现
- ⚠️ 部分: 基本实现但需完善
- ❌ stub: 仅占位符

---

## 7. 实现差异分析

### 7.1 需要完善的工具方法

**ToolOrchestrator.cpp 中stub实现:**

1. **TaskList** (line ~1678): 返回固定文本 `[TaskList] Task tracking system active`
   - 需实现: 维护任务列表状态, 返回JSON格式任务清单

2. **NotebookEdit** (line ~1720): 返回假结果
   - 需实现: 解析 .ipynb JSON, 替换cell内容

3. **ListMcpResources** (line ~1760): 返回固定文本
   - 需实现: 调用 McpClientManager 获取资源列表

4. **ReadMcpResource** (line ~1750): 返回简单提示
   - 需实现: 调用 McpClientManager 读取资源内容

5. **Skill** (line ~1660): 简化实现
   - 需实现: SKILL.md 文件读取和技能执行

### 7.2 测试覆盖度

| 测试文件 | 测试模块 | 当前状态 |
|---------|---------|---------|
| test_hooks.cpp | HookTypes, HookConfig, HookExecutor | ✅ 有测试 |
| test_tools.cpp | ToolRegistry, ToolOrchestrator | ✅ 有测试 |
| test_mcp.cpp | McpClientManager | ✅ 有测试 |
| test_memory.cpp | MemoryIndex, AutoDream | ✅ 有测试 |
| test_core.cpp | QueryEngine, QueryLoop | ✅ 有测试 |
| test_infra.cpp | SessionManager, ProcessRunner | ✅ 有测试 |
| test_subagent.cpp | SubAgentManager | ✅ 有测试 |
| test_e2e.cpp | 端到端流程 | ✅ 有测试 |
| smoke.cpp | 快速冒烟 | ✅ 有测试 |

**缺失测试:**
- BashClassifier 单独测试
- PermissionEngine 详细测试
- CostTracker 测试
- StabilityWatchdog 测试
- StreamingToolExecutor 测试

---

*本文档基于对 local-ace (TypeScript/Bun) 和 cpp-agent (C++17/CMake) 源码的完整分析生成*
