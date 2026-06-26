# 弱模型支持方案分析：local-ace vs MiMo-Code vs cpp-agent

> 基于 local-ace（TypeScript）、MiMo-Code（opencode 分支）源码逐项比对，结合 cpp-agent 现状，分析两工程是否有针对弱模型的措施，以及增强弱模型指令遵循 + 一次性完成"创建系统→单元测试→工程测试→测试修复"完整流程能力的解决方案。

---

## 一、核心结论速览

**两个参考工程都系统性针对弱模型设计了措施，但策略迥异：**

| 维度 | local-ace | MiMo-Code | cpp-agent 现状 |
|---|---|---|---|
| 弱模型识别 | 仅 `LOCALMODEL_VALIDATION_MODEL` 环境变量门控 | **按模型 id 选系统提示词文件**（gpt/gemini/claude/kimi/trinity/default） | 有 `DetectModelFamily`(Claude/Qwen/Gemma/MiMo)，但**只调温度，不改提示词** |
| 指令遵循 | LLM 验证器 rewrite/block/retry | goal judge 停止门 + task-board 停止门 + text-loop 恢复 | 有 `VerifyGoalCompletion`(LLM judge)，但**默认关闭**，且无任务锚点防跑题 |
| 防死循环 | 仅提示词一句话 + 验证器 block | **三层**：doom_loop(3×) + text-loop(mild→strong→terminate) + repeated-step(签名去文本) | 有 duplicate/excessive_exploration 检测（本次修复D已加固），**无 text-loop 检测** |
| 创建→测试→修复闭环 | 对抗式验证子agent + coordinator 四阶段 | compose skill：spec-anchored 两阶段评审 + 证据门 | 提示词有 Write-Run-Verify，但**无结构化评审/子agent分工** |
| 长上下文记忆 | 无专门机制 | **checkpoint-writer/dream/distill 三个系统子agent** + 验证器驱动自修正循环 | 有 5 层压缩引擎，**无 checkpoint 结构化记忆** |
| 跨会话记忆 | 无 | dream（整合）+ distill（把重复工作流打包成 skill） | 有 AutoDream，**无 distill 工作流沉淀** |

**关键发现**：cpp-agent 在"模型族检测"和"压缩引擎"上其实比 local-ace 更完善，但**最致命的缺口**是——
1. **无任务锚点（task anchor）防跑题**：这正是测试中 Gemma 跑题建 task_manager 而非 md2html 的根因。
2. **系统提示词不随模型族变化**：MiMo-Code 对弱模型用更长、更指令化的提示词，cpp-agent 对所有模型用同一段。
3. **VerifyGoalCompletion 默认关闭**：弱模型最需要的"独立判官"恰恰没开。

---

## 二、local-ace 的弱模型方案

### 2.1 唯一的显式弱模型门控：LLM 验证器

local-ace **没有模型族检测**（grep 全树，`ModelFamily` 只映射 Claude 的 sonnet/opus/haiku，无 Qwen/Gemma/MiMo）。它对弱模型的唯一显式支持是 `LOCALMODEL_VALIDATION_MODEL` 环境变量（`services/validation/index.ts:6`）：

- **门控**：`isValidationEnabled()` 仅当该 env 设置时返回 true，env 名字本身就是"本地模型验证"。
- **机制**（`validateAssistantDraft`）：用**独立的验证器模型**审查弱模型的输出，可做 `text_correction`（改写文本）、`tool_interventions`（rewrite 重写工具调用 / block 拦截）、`final_response_action=retry_from_tools`（强制重试）。
- **关键设计**：开启验证后，**弱模型的所有输出在流式阶段被抑制**（`query.ts:830`），验证器改写/拦截后才放出——弱模型永远"先审后发"。
- **缺陷**：验证器需比主模型强才有意义；同级验证（如 Gemma 验证 Gemma）反而更差（cpp-agent 的 main.cpp 注释已认识到这点，故默认 Peer 层级跳过 LLM 验证）。

### 2.2 对抗式验证子 agent（防假完成）

`tools/AgentTool/built-in/verificationAgent.ts` ——一个**只读**的对抗验证子 agent，专门针对弱模型的"假完成"模式：
- 系统提示词直击弱模型的两个失败模式（`verificationAgent.ts:12-13`）："verification avoidance（找理由不跑验证，读代码、叙述会测什么、写 PASS 了事）"和"被前 80% 诱惑"。
- 强制输出 `VERDICT: PASS|FAIL|PARTIAL`，且每个检查必须有 `Command run:` 块——"没有 Command run 的 PASS 是 skip"。
- 主 agent 在报告完成前**必须** spawn 这个验证器，FAIL 则修复后重启验证器，直到 PASS（`prompts.ts:390-395`）。

### 2.3 任务跟踪 + 提醒再注入

- TodoWrite/TaskCreate 两个工具，提示词强调"主动用、立即标记完成、不批量"。
- `attachments.ts` 的 `todo_reminder`/`task_reminder`/`verify_plan_reminder`/`critical_system_reminder` 周期性把任务列表/计划重新注入，防止弱模型丢线。
- **但无"原始目标重注入"**：grep `Recent execution memory`/`execution memory` 零匹配——任务锚定靠 todo/plan 间接实现。

### 2.4 coordinator 四阶段分工

`coordinatorMode.ts:198-237` 把复杂任务拆成 Research→Synthesis→Implementation→Verification 四阶段，每阶段可并行 spawn worker 子 agent。第 5 节（251-336）强制 coordinator 自己综合发现，防"懒委托"。但这是 Claude 向设计，弱模型未必能胜任 coordinator 角色。

### 2.5 局限

- **无模型族感知提示词**、**无专门的弱模型防循环 guard**（防循环靠验证器 block + 一句提示词"被拒就不要重试"）。
- 验证器依赖"更强的验证模型"，对纯本地弱模型场景（无云端强模型）不适用。

---

## 三、MiMo-Code 的弱模型方案（明显更深入）

### 3.1 按模型选系统提示词文件（最直接的弱模型适配）

`session/system.ts:19-33` 按模型 id 子串选提示词文件：
```
gpt-4/o1/o3 → beast.txt（"MUST iterate… NEVER end turn without truly solving"）
gpt → gpt.txt
gemini → gemini.txt
claude → anthropic.txt
kimi → kimi.txt（95 行超长规范：工具规则、并行调用、最小改动、失败重测）
default → default.txt（未知/本地模型回退，强调 task 工具 + 委托）
```
**这是 cpp-agent 最该补的**：弱模型用更长、更指令化、更明确的提示词。

### 3.2 三层防死循环（远超 local-ace）

1. **doom_loop 权限门**（`processor.ts:27,400-427`）：连续 3 次相同工具+相同 JSON 输入→走权限 ask，系统子 agent 非交互直接 fail-clean。
2. **text-loop 恢复**（`prompt/text-loop-recovery.ts`）：归一化（去大小写/去"let me"填充词）后比对最近 3 条输出，相同则注入 mild 提示→再犯注入 strong 提示→第三次**直接终止会话**。这是 cpp-agent 完全没有的。
3. **repeated-step 签名**（`prompt.ts:113-118,148-156`）：签名**刻意排除文本**（因为弱模型每次叙述不同但动作相同），只取工具名+输入，连续 3 次相同签名→注入"你重复了，换策略"。

### 3.3 goal judge + task-board 双停止门（防假完成）

- **goal judge**（`goal.ts:16-31`）：设置 goal 后，主循环**拒绝停止**，直到独立判官模型（只读 transcript，不干活，"冷判断"）裁定 PASS。判官要求 transcript 证据，不轻信 agent 自述"不可能"。`MAX_GOAL_REACT=12` 兜底。**cpp-agent 有等价物 `VerifyGoalCompletion`，但默认关闭**。
- **task-board 门**（`prompt.ts:1854-1923`）：停止前先列未完成任务，有未完成就注入 synthetic user turn 逼模型关闭。**在 goal 门之前跑**，因为"未完成任务会污染 goal 裁决"。

### 3.4 checkpoint-writer / dream / distill 三系统子 agent（最独特）

这是 MiMo-Code 区别于 stock opencode 的核心，专治弱模型的长上下文/跨会话问题：

- **checkpoint-writer**（`agent.ts:285-315`）：上下文增长时，**独立子 agent** 把旧历史总结成结构化 `checkpoint.md`（强制段落：Execution context/Live resources/Session metadata/Discovered/Dead ends），喂回主 agent。弱模型小上下文窗口也能保持持久状态。
  - **验证器驱动自修正循环**（`checkpoint-splitover.ts` + `checkpoint-validator.ts`）：子 agent 停止前跑验证器，若结构不合规（缺段落、重复发现、填充式"Next: continue"）→ `output.continue=true` + 反思消息→**重新进入子 agent 直到通过**。把"弱模型产出草率"从静默失败变成强制自修正。
- **dream**（`agent.ts:315-342`）：每周后台跑，从 SQLite 原始轨迹 + 现有 memory 文件整合**高信号持久记忆**（"密度比完整性重要"）。
- **distill**（`agent.ts:342-368`）：每月后台跑，把重复的手动工作流**打包成 skill/agent/command**，直接降低未来弱模型负担。

### 3.5 compose skill：结构化创建→测试→修复（最完整的一次性工程流程）

`skill/compose/.bundle/subagent/SKILL.md` 是旗舰工作流：
- **spec-anchored 评审门**：绑定 task→把 spec 相关段原文注入 implementer（"implementer 永不读 spec 本身"）→两阶段证据门评审：
  - Phase 1：只用 spec + git diff（**不看 implementer 报告**，因为"报告会把评审者锚定到确认而非发现遗漏"）。
  - Phase 2：仅当 Phase 1 标记问题时跑。
  - 通过条件：每个范围内声明都 `status:pass` 且有可验证证据（测试名/命令输出/file:line），"无证据的结构化 pass 视为 fail"。
- **模型分级**（`:133-151`）：机械任务用便宜模型，集成用标准模型，设计/评审用最强模型，且**评审者必须至少与实现者同等强**（"弱评审者共享盲点，橡皮图章同样的误读"）。
- **状态路由**（`:154-169`）：DONE/DONE_WITH_CONCERNS/NEEDS_CONTEXT/BLOCKED，"绝不忽略升级或在不改东西时强制同一模型重试"。

### 3.6 弱模型工具调用容错

- **动态 subagent_type enum**（`actor.ts:299-314`）：取代裸 `z.string()`，因为"模型无法自省有哪些 agent（三次测试零子 agent spawn 的根因）"。
- **`.meta({type:"object"})` 包裹**（`actor.ts:447-465`）：防弱模型（mimo-v2.5-pro）把整个 envelope 字符串化导致 zod 失败。
- **recoverActorArgs**（`actor.ts:222-263`）：把模型错误放在顶层的 `{subagent_type,description,prompt}` 提回操作 envelope，降级而非硬失败。
- **伪造 task_id 容错**（`actor.ts:677-695`）：验证失败降级为 ad-hoc + 说明，而非硬失败。

---

## 四、cpp-agent 现状对标（已具备 vs 缺口）

### 4.1 已具备（部分比 local-ace 更好）
- ✅ `DetectModelFamily`(Claude/Qwen/Gemma/MiMo) + `ModelClient` 按族调温度——**比 local-ace 更细**。
- ✅ 5 层压缩引擎（Budget→Snip→Micro→Collapse→Autocompact）——比两参考都完整。
- ✅ `VerifyGoalCompletion`（LLM goal judge）——对标 MiMo-Code goal gate，但**默认关闭**。
- ✅ `executionContract`（LLM 生成目标契约）——对标 local-ace 验证器，但**仅 Stronger 层级生成**（弱模型恰恰跳过）。
- ✅ `BuildRecentExecutionMemory` 重复错误/重复成功检测——对标 MiMo-Code repeated-step。
- ✅ 本次修复A-D：权限模式可配、settings 规则、死循环根治、doom_loop 加固。
- ✅ 系统提示词已含 Write-Run-Verify / Task Management / Error Repair / Completion Reporting 四段。

### 4.2 关键缺口（按对弱模型影响排序）

| # | 缺口 | 影响 | 参考来源 |
|---|---|---|---|
| **G1** | **无任务锚点防跑题** | 测试中 Gemma 跑题建 task_manager 而非 md2html 的直接根因；`BuildMessagesForTurn` 不重注入原始目标 | MiMo-Code goal gate + local-ace plan reminder |
| **G2** | **系统提示词不随模型族变化** | 弱模型用与 Claude 相同的提示词，缺少更长、更指令化、更明确的约束 | MiMo-Code `system.ts:19-33` 按模型选提示词文件 |
| **G3** | **VerifyGoalCompletion 默认关闭** | 弱模型最需要的"独立判官"恰恰没开；且 `executionContract` 仅 Stronger 层级生成，弱模型用不上 | MiMo-Code goal gate 默认开 |
| **G4** | **无 text-loop 检测** | 弱模型退化式重复文本（每次叙述不同）无法被现有 duplicate 检测器捕获 | MiMo-Code `text-loop-recovery.ts` |
| **G5** | **无结构化创建→测试→修复评审** | 一次性大工程靠提示词 Write-Run-Verify 一句话，无两阶段证据门评审 | MiMo-Code compose skill |
| **G6** | **无 checkpoint 结构化记忆** | 长任务靠压缩（有损摘要），弱模型丢失关键决策上下文 | MiMo-Code checkpoint-writer |
| **G7** | **无 distill 工作流沉淀** | 重复工程模式无法沉淀为可复用 skill，每次都从零 | MiMo-Code distill |

---

## 五、cpp-agent 增强解决方案（按性价比排序）

### 方案 1【高性价比·防跑题】任务锚点重注入（补 G1）
**位置**：`QueryLoop::BuildMessagesForTurn`（`QueryLoop.cpp:1590`）

每轮把原始用户目标（首条非 meta user 消息）作为 system meta 消息插到最前，与 `BuildRecentExecutionMemory` 并列：
```
[Task Anchor — do not lose sight of this]
<原始用户目标前 1500 字>
You MUST stay focused on THIS task. Do not substitute a different project,
file, or goal. If you are about to create files, verify their names/paths
match what this task specifies.
```
- 无需 LLM、无额外成本，每轮强提醒。
- 直接对治测试中的跑题（Gemma 建了 task_manager 而非 md2html）。
- 参考：MiMo-Code goal gate 的"拒绝停止直到 goal 满足" + local-ace plan reminder 周期重注入。

### 方案 2【高性价比】模型族感知系统提示词（补 G2）
**位置**：`RuntimePolicy.cpp:BuildWorkspaceSystemPrompt`

按 `DetectModelFamily(model)` 追加族专属段落：
- **Gemma/Qwen/MiMo（弱本地模型）**：追加更指令化的约束——"严格按用户指定的文件名/路径创建，不要自行替换项目名"、"一次只做一个任务，做完用 TodoWrite 标记"、"被拒绝的工具不要重试，换方法或报告"、"不要叙述计划，直接行动"。
- **Claude**：保持现有简洁提示词。
- 参考：MiMo-Code `system.ts:19-33` 按模型选提示词文件 + kimi.txt 的超长规范。

### 方案 3【中性价比】默认开启弱模型 goal 验证 + 契约降级生成（补 G3）
**位置**：`QueryLoop::VerifyGoalCompletion` + `GenerateExecutionContract`

- `VerifyGoalCompletion` 默认对 Gemma/Qwen/MiMo 族**开启**（现在默认关）。
- `GenerateExecutionContract`：弱模型无 Stronger 验证器时，**用主模型自己生成契约**（temperature=0，作为结构化目标重述），而非跳过。契约生成后作为 task anchor 的一部分注入。
- `MAX_GOAL_REACT` 设较小值（如 6）防弱模型在 goal 门里空转。
- 参考：MiMo-Code goal judge 默认开 + `MAX_GOAL_REACT=12`。

### 方案 4【中性价比】text-loop 检测（补 G4）
**位置**：新增 `TextLoopDetector` + `HandleNoToolContinuation`

- 维护最近 5 条归一化输出（去大小写/去"let me/i'll"填充），连续 3 条相同→注入 mild 恢复提示；再犯→strong 提示；第三次→`terminalReason=text_loop` 终止。
- 现有 duplicate 检测器只看工具指纹，捕获不到"文本重复但工具不同"的退化。
- 参考：MiMo-Code `prompt/text-loop-recovery.ts` 完整方案。

### 方案 5【较大工程】结构化两阶段评审（补 G5）
**位置**：复用 `SubAgentManager` + 新增 `VerificationSubAgent`

- 仿 local-ace `verificationAgent.ts` + MiMo-Code compose：主 agent 报告完成前 spawn 只读验证子 agent，要求 `VERDICT:PASS/FAIL` + 每检查带 `Command run:` 证据。
- FAIL→修复→重启验证，直到 PASS 或达 `MAX_VERIFY_REACT`。
- cpp-agent 已有 `SubAgentManager` 基础设施，主要工作是写验证子 agent 的系统提示词 + 接入 stop 流程。
- 这是"一次性完成完整流程"能力提升最大的方案，但工程量最大。

### 方案 6【较大工程】checkpoint-writer 结构化记忆（补 G6）
仿 MiMo-Code：压缩触发时由子 agent 产出结构化 `checkpoint.md`（含 Discovered/Dead ends 段），而非纯有损摘要。cpp-agent 已有 `AutoDream` + 压缩引擎，可在此基础上加结构化段落约束 + 验证器自修正循环。

### 方案 7【长期】distill 工作流沉淀（补 G7）
仿 MiMo-Code distill：周期性扫描会话轨迹，把重复工程模式打包成可复用提示词片段/skill。工程量大，建议作为长期演进项。

---

## 六、推荐落地顺序

针对"增强弱模型一次性完成复杂系统创建+单元测试+工程测试+测试修复完整流程"这一目标，按 **投入产出比** 推荐顺序：

1. **方案 1（任务锚点）+ 方案 2（族感知提示词）**——零额外 LLM 成本、改动小、直接对治跑题和指令遵从，**应优先做**。这两项就能显著改善测试中 Gemma 的跑题问题。
2. **方案 3（默认开 goal 验证 + 契约降级）**——弱模型最需独立判官，且契约即结构化目标，与方案 1 协同。
3. **方案 4（text-loop 检测）**——补齐防循环最后一块，改动中等。
4. **方案 5（两阶段评审子 agent）**——对"一次性完整流程"提升最大但工程量大，建议在前 4 项验证有效后再做。
5. 方案 6/7 作为长期演进。

**核心判断**：cpp-agent 框架底子好（模型族检测、压缩引擎、子 agent 基础设施都优于 local-ace），主要缺"把已有能力对弱模型默认开启 + 任务锚定"。方案 1+2+3 三项（均改动小、无新依赖）即可把"跑题+假完成+指令遵从差"三大弱模型顽症大幅压制，使 Gemma 这类弱模型具备初步的真实复杂工程使用条件；方案 5 是达到"一次性完整流程"的决定性补强。
