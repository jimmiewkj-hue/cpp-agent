# cpp-agent

VS2017 + CMake Native C++ Agent Kernel，从 `local-ace` TypeScript 工程重构而来。

## Phase 3 — Stability Architecture (current)

所有核心模块已落地为可编译骨架，新增稳定性架构层。

| Module                | Header                      | Source                      | Status      |
|-----------------------|-----------------------------|-----------------------------|-------------|
| AgentTypes            | `core/AgentTypes.h`         | (header-only)               | 骨固定型    |
| StateTypes            | `core/StateTypes.h`         | `core/StateTypes.cpp`       | 新增强      |
| QueryEngine           | `core/QueryEngine.h`        | `core/QueryEngine.cpp`      | 增强        |
| QueryLoop             | `core/QueryLoop.h`          | `core/QueryLoop.cpp`        | 阶段状态机  |
| ModelClient           | `api/ModelClient.h`         | `api/ModelClient.cpp`       | 骨架        |
| SideQueryClient       | `api/SideQueryClient.h`     | `api/SideQueryClient.cpp`   | 骨架        |
| ToolRegistry          | `tools/ToolRegistry.h`      | `tools/ToolRegistry.cpp`    | 新增        |
| ToolOrchestrator      | `tools/ToolOrchestrator.h`  | `tools/ToolOrchestrator.cpp`| 增强        |
| PermissionEngine      | `permissions/PermissionEngine.h`| `permissions/PermissionEngine.cpp`| 增强 |
| ProcessRunner         | `infra/ProcessRunner.h`     | `infra/ProcessRunner.cpp`   | 骨架        |
| SessionManager        | `infra/SessionManager.h`    | `infra/SessionManager.cpp`  | 结构化快照  |
| StabilityWatchdog     | `infra/StabilityWatchdog.h` | `infra/StabilityWatchdog.cpp`| 新增       |
| MemoryIndex           | `memory/MemoryIndex.h`      | `memory/MemoryIndex.cpp`    | 骨架        |
| McpClientManager      | `mcp/McpClientManager.h`    | `mcp/McpClientManager.cpp`  | 半实装      |
| SubAgentManager       | `agents/SubAgentManager.h`  | `agents/SubAgentManager.cpp`| 执行器调度  |
| CLI Entry             | (none)                      | `app/main.cpp`              | 增强入口    |
| Smoke Tests           | (none)                      | `tests/unit/smoke.cpp`      | 25 用例     |

### Phase 3 关键变更

**模块完备性补全：**
- 新增 `ToolRegistry`：工具 schema 注册与 concurrency safe / readOnly / maxResultSizeChars 查询，替代硬编码判定
- 新增 `StateTypes`：`AgentConfig`、`DenialTrackingState`、`AbortError`、`FallbackTriggeredError`、`SessionMetadata`
- `PermissionEngine` 升级：加入 `DenialTrackingState`（maxConsecutive=3/maxTotal=20 熔断）、`autoModeAllowlistedTools`（跳过分类器）、`FAIL_CLOSED_GATE` 策略
- `QueryEngine` 增强：接受 `AgentConfig` + `ToolRegistry` + `SessionManager` + `StabilityWatchdog` 注入，支持 `RunTurnWithRecovery()`

**稳定性新架构（四层中的 infra 层强化）：**
- `StabilityWatchdog`：独立监控线程、心跳检测、健康检查、自动恢复、资源监控回调
- `SessionManager`：消息追加、强类型二进制快照 (`snapshot.bin`) + 旧版文本快照兼容恢复、转录日志 (`transcript.txt`)
- `QueryEngine` 每轮更新 session metadata 并触发 watchdog heartbeat

**本轮迭代新增：**
- `McpClientManager` HTTP transport：补齐 `Mcp-Session-Id` / `X-Mcp-Client-Session-Id` 生命周期字段、WinHTTP 连接复用、streamable-http (`application/json` / `text/event-stream` / `application/x-ndjson`) 响应解析、404 `session expired` 重连恢复
- `SessionManager`：从行协议升级为版本化 TLV 二进制快照，保留 `snapshot.txt` 作为兼容/调试侧车，`RestoreFromDisk()` 优先读二进制，失败再回退旧格式
- `SubAgentManager`：新增执行器槽位、任务 checkpoint、执行器故障感知、恢复后基于优先级和负载的重分配，不再只做简单状态回退

**四层架构确认：**
| 层 | 模块 | 现状 |
|---|------|------|
| **Core** Agent 内核 | `QueryEngine`, `QueryLoop`, `AgentTypes`, `StateTypes` | 完整骨架 |
| **Tools & Permissions** | `ToolRegistry`, `ToolOrchestrator`, `PermissionEngine` | 完整骨架 |
| **Infrastructure** | `ProcessRunner`, `SessionManager`, `StabilityWatchdog`, `MemoryIndex`, `McpClientManager`, `SubAgentManager` | 骨架就绪 |
| **API** | `ModelClient`, `SideQueryClient` | 骨架就绪 |

**稳定性 8-Hour 连续执行能力：**
- ✅ 心跳检测 (5s 间隔, 30s 超时)
- ✅ 连续失败熔断 (maxConsecutiveFailures=10)
- ✅ 自动恢复回调 + 快照保存
- ✅ Session 快照持久化与断点恢复
- ✅ Transcript 转录日志
- ✅ ProcessRunner 超时杀进程 + 5s 优雅等待
- ✅ PermissionEngine 熔断与 fail-closed 策略
- ✅ Turn 计数与 session metadata 追踪
- ✅ 子任务执行器状态持久化与恢复后重调度
- ✅ MCP HTTP 会话状态跟踪与最小 streamable-http 兼容

### 尚未实现（后续阶段）

- 真实的 SSE 流式请求与 `thinking` block
- `applyToolResultBudget` / `microcompact` / `autocompact` / `snipCompact` / `contextCollapse` / `reactiveCompact`
- `Validator Layer` 的结构化 JSON 解析与 correction/block/retry 逻辑
- `Stop Hooks` / `executeAutoDream` / `executeTaskCompletedHooks`
- `Fallback` 模型切换与 `413` 恢复链
- `classifyYoloAction` 安全分类器
- 记忆系统的 Auto-Dream 四阶段整合
- 更完整的 MCP HTTP/SSE/WebSocket 协议兼容层
- 子代理系统真实 worktree 创建/清理与底层执行器进程模型
- 终端 UI（FTXUI 或纯控制台）
- 真实多工具实现 (BashTool, FileReadTool, FileWriteTool 等)

## 构建

```powershell
cmake -S . -B build -G "Visual Studio 15 2017 Win64"
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## 运行

```powershell
.\build\Release\agent_cli.exe
```

启动后会先确认当前启动目录是否作为“可信工程工作区”。

- 选择 `yes` 后：当前目录作为工作区根目录，相对路径读写都落在该工程目录下
- 会话状态与记忆目录放在工作区下的隐藏目录 `.cpp-agent/`
- 创建新文件默认应写入工作区中的真实项目路径，而不是状态目录
- 若要引用工程外文件，需要提供本地绝对路径，或先将文件拷贝到工程目录中

当前推荐的状态目录布局：

- `.cpp-agent/session/` - 会话快照与 transcript
- `.cpp-agent/memory/` - 持久记忆文件
- 工程产物文件 - 直接写入工作区内对应相对路径

输出示例：
```
cpp-agent bootstrap
total_messages=5
total_turns=1
watchdog_healthy=1
  role=user blocks=1
  role=asst blocks=1
  role=asst blocks=1
  role=user blocks=1
  role=asst blocks=1
```

生成的持久化文件：
- `.cpp-agent/session/snapshot.pb` — 结构化 session 快照
- `.cpp-agent/session/snapshot.txt` — 兼容/调试快照镜像
- `.cpp-agent/session/transcript.txt` — 消息转录日志
- `.cpp-agent/memory/MEMORY.md` — 记忆索引
