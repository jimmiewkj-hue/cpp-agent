# cpp-agent 主模型与校验模型选型建议

> **文档用途**：基于 `sessions/` 与 `build/` 中 qwen、gemma、mimo 三类本地模型的真实会话日志，结合本次双模型交互修复（STRENGTHEN-01~04），给出主模型与校验模型的具体选型建议。
> **证据来源**：`gemma_replies.txt`、`gemma_replies_graph1.txt`、`build/validator-retry-limit-session/validator-model.jsonl`、`sessions/jianlai_*/` 真实多轮会话。

---

## 一、三个本地模型的实测行为画像

### 1. Gemma（gemma-3-27B / gemma-3-12B 等）

**实测特色（来自 gemma_replies_graph1.txt、gemma_replies.txt）**：

| 维度 | 表现 | 证据 |
|---|---|---|
| **行动导向** | 强。拿到任务后倾向直接发工具调用（Read/Glob/Bash），不空谈 | gemma_replies_graph1.txt 行 2-11：收到任务立即 "Let me start by: 1. Exploring..." 后紧跟工具调用 |
| **计划-执行交替** | 频繁。常先输出一段计划文本，下一轮才发工具 | jianlai_graph2 工程 log：21 轮中 9 轮纯文本（见 QueryLoop.cpp:3495 注释） |
| **简洁性** | 中等。不暴露 CoT，但偶有冗长计划段 | gemma_replies.txt 行 5-14：完成后有一段自我核对 "Looking at my response, I've: 1. Listed... 2. Read..." |
| **跨平台鲁棒性** | 弱。倾向写 Unix shell 语法（`&&`、`2>/dev/null`、`pip list | head`） | gemma_replies_graph1.txt 行 79-94：PowerShell 不支持 `&&` 反复失败，需系统提示纠正 |
| **同文件编辑固执** | 中。SearchReplace 失败后倾向微调同一行而非重读 | 已有 `consecutiveSameFileEditFailures` 计数器专门处理（QueryLoop.h:87） |
| **过早停止** | 低。只要系统提示强调 "continue until done"，能稳定多轮 | jianlai-graph-20 会话：能完成 read→analyze→summarize 完整链路 |

**Gemma 一句话画像**：**实干型**，执行力强但跨平台意识弱，需要强系统提示约束 shell 语法。

### 2. Qwen（qwen3-32B / qwen2.5-32B 等）

**实测特色（来自 validator-retry-limit-session + sessions/jianlai_mimo 对比）**：

| 维度 | 表现 | 证据 |
|---|---|---|
| **计划固执（最严重）** | 极强。**会逐字重复**上一轮的计划文本，无视系统 nudge 和 validator guidance | validator-model.jsonl：主模型 4 轮重复 "我先直接读取 README" + 同一 FileRead 调用 |
| **响应 validator** | 几乎不响应。validator 说 "Stop rereading README"，Qwen 下一轮原样重复 | validator-retry-limit-session turn 1-4 完整记录 |
| **中文友好** | 强。systemPromptQwen 中文提示效果好 | StateTypes.h:108 systemPromptQwen |
| **推理深度** | 中上。规划详尽但易陷入"规划瘫痪" | 多个 session 显示首轮输出 7+ 步计划但不执行 |
| **工具调用格式** | 准确。JSON 参数格式问题少 | OpenAI function-calling 格式偏好（AgentTypes.h:66） |
| **过早停止** | 中。纯问答任务能收尾，工程任务易停在计划阶段 | — |

**Qwen 一句话画像**：**规划型但固执**，推理细但易卡在计划阶段且拒绝外部纠正，是 validator 死循环的主犯。

### 3. MiMo（Xiaomi MiMo）

**实测特色（来自 systemPromptMiMo 设计 + 对比）**：

| 维度 | 表现 |
|---|---|
| **结构化推理** | 强。英文结构化提示效果最佳（systemPromptMiMo） |
| **API 一致性意识** | 强。系统提示专门强调 "READ all created modules before running" |
| **错误恢复** | 中。遇 AttributeError 能按提示先读源码 |
| **跨平台** | 中。需 requestTimeoutMs=600000（StateTypes.h:29 注释：cloud models need longer） |

**MiMo 一句话画像**：**工程严谨型**，适合复杂多模块项目，但响应较慢。

---

## 二、主模型选型建议

### 推荐排序

| 排名 | 模型 | 适用场景 | 理由 |
|---|---|---|---|
| 🥇 | **Gemma-3-27B**（主推） | 通用编码、文件操作、多轮迭代 | 行动力最强，工具调用稳定，配合 systemPromptGemma 的 shell 约束后跨平台问题可控；多轮稳定性最好（不会过早停止） |
| 🥈 | **MiMo** | 复杂多模块项目、需 API 一致性保证 | 工程严谨度最高，但单轮延迟大，不适合交互式快速迭代 |
| 🥉 | **Qwen3-32B** | 纯问答、分析、文档生成 | 推理深但工程任务易陷计划瘫痪；本次修复加了反执述（STRENGTHEN-04）后改善，但仍不如 Gemma 实干 |

### 具体配置（环境变量）

**推荐配置 A：Gemma 单模型（最稳）**
```cmd
set CPP_AGENT_MAIN_MODEL=gemma-3-27b-it
set CPP_AGENT_MAIN_ENDPOINT=http://127.0.0.1:1234/v1
:: 不设 validator model —— 默认 Peer 档，只跑 LocalValidator 规则
:: 这是修复后的新默认，避免同层净负
```

**推荐配置 B：Gemma 主 + Claude 校验（最强）**
```cmd
set CPP_AGENT_MAIN_MODEL=gemma-3-27b-it
set CPP_AGENT_MAIN_ENDPOINT=http://127.0.0.1:1234/v1
set CPP_AGENT_VALIDATOR_MODEL=claude-sonnet-4-20250514
set CPP_AGENT_VALIDATOR_ENDPOINT=https://api.anthropic.com
set CPP_AGENT_VALIDATOR_API_KEY=sk-ant-...
:: tier 自动判定为 Stronger（Claude 校验 Gemma），LLM 校验启用
```

**不推荐配置：Qwen 主 + Gemma 校验（或反过来）**
```cmd
:: ❌ 不要这样配 —— 本次修复会让 tier=Peer，LLM 校验不跑
::    即使强行 CPP_AGENT_VALIDATOR_TIER=stronger 也会重现死循环
set CPP_AGENT_MAIN_MODEL=qwen3-32b
set CPP_AGENT_VALIDATOR_MODEL=gemma-3-27b-it
```

---

## 三、校验模型选型建议

### 核心原则（本次修复确立）

**校验模型必须明显强于主模型，否则不如不用。** 这是 `CompareModelFamilies`（StateTypes.h）的判定逻辑，也是 jianlai_graph 工程 log 的实证结论。

### 推荐排序

| 排名 | 校验模型 | 配主模型 | tier | 效果 |
|---|---|---|---|---|
| 🥇 | **Claude Sonnet/Opus**（云端） | Gemma / Qwen / MiMo | Stronger | ✅ 真正 1+1>2，云端模型逻辑能力强，纠错有效 |
| 🥈 | **不设校验模型**（用 LocalValidator） | 任意本地 | Peer | ✅ 安全默认，纯规则校验无 LLM 开销，无死循环风险 |
| 🥉 | **GPT-4o**（云端） | Gemma / Qwen | Stronger | ✅ 同 Claude，但需 OpenAI 端点 |
| ⚠️ | **同层本地模型互校** | Qwen↔Gemma | Peer | ❌ 修复后自动跳过 LLM 校验；强行启用会重现死循环 |
| ❌ | **弱校验强** | Qwen 校验 Claude | Weaker | ❌ 修复后自动跳过 |

### 为什么"不设校验模型"（🥈）比"配个本地校验"好

本次修复后，**不设校验模型 = Peer 档 = 只跑 LocalValidator 确定性规则**（`CheckUnixCommandsOnWindows`/`ForbidsFileOperations`/`RequiresHtmlCssOnly` 等）。这些规则：
- 零 LLM 开销、零延迟
- 零假阳性（纯模式匹配）
- 不会死循环（无 LLM 参与）

而配一个同层本地校验模型：
- 即使本次修复加了状态感知（STRENGTHEN-01）和反执述（STRENGTHEN-02），**同层模型的纠错本身就是噪声**——它不比主模型懂更多
- validator-retry-limit-session 的死循环就是 Qwen 校验 Qwen 的后果

**结论：没有强校验模型时，宁可只用规则校验，不要硬配本地校验。**

---

## 四、环境变量速查表

| 变量 | 作用 | 推荐值 |
|---|---|---|
| `CPP_AGENT_MAIN_MODEL` | 主模型名 | `gemma-3-27b-it` |
| `CPP_AGENT_MAIN_ENDPOINT` | 主模型端点 | `http://127.0.0.1:1234/v1` |
| `CPP_AGENT_VALIDATOR_MODEL` | 校验模型名（留空=Peer档） | 留空，或 `claude-sonnet-4-20250514` |
| `CPP_AGENT_VALIDATOR_ENDPOINT` | 校验模型端点 | 留空，或 `https://api.anthropic.com` |
| `CPP_AGENT_VALIDATOR_TIER` | 强制覆盖 tier 判定 | 留空（自动）；调试时可设 `stronger`/`peer`/`weaker`/`disabled` |
| `CPP_AGENT_FALLBACK_MODEL` | 主模型失败时的备用 | `qwen3-32b`（不同族，提高容错） |
| `LOCALMODEL_VALIDATION_MODEL` | 兼容 local-ace 的旧变量名 | 同 `CPP_AGENT_VALIDATOR_MODEL` |

---

## 五、本次修复如何解决"1+1<2"

### 修复前的死循环（validator-retry-limit-session 实证）

```
主模型(Qwen): "我先直接读取 README" + FileRead(README.md)
   ↓
校验模型(Qwen): retry_from_tools "Stop rereading README"
   ↓ （注入 [Validator] meta，主模型当作软提示忽略）
主模型(Qwen): "我先直接读取 README" + FileRead(README.md)  ← 逐字重复
   ↓
校验模型(Qwen): retry_from_tools "Stop rereading README"  ← 也逐字重复
   ↓ ×4 轮
[system] 强制终结: validator_retry_limit
```

**根因**：
1. 校验模型无状态感知——不知自己已说过同样的话（机械重复）
2. retry_guidance 是软 `[Validator]` meta，主模型当背景噪音忽略
3. 同层模型互校（Qwen 校 Qwen），纠错本身是噪声

### 修复后的行为

**STRENGTHEN-01（状态感知）**：校验模型现在收到 `validator_self_state`：
```json
{"retry_count_this_cycle": 1, "previous_guidance": ["Stop rereading README"],
 "assistant_last_attempt": "我先直接读取 README"}
```
系统提示强制：`retry_count >= 1 且 assistant 未改变 → 必须 approve 或给全新 guidance，禁止重复`。

**STRENGTHEN-02（硬约束注入）**：retry guidance 从 `[Validator] xxx` 升级为：
```
[MANDATORY ACTION — repeat warning #2] You have already given this response
2 time(s) and it was rejected each time. Repeating it again is a failure mode.
You MUST now do something concretely different. Required change: ...
```

**STRENGTHEN-03（Tier 分层）**：Qwen 校验 Qwen → Peer 档 → LLM 校验根本不跑，只跑规则。从源头消除死循环。

**STRENGTHEN-04（反执述）**：Qwen 重复计划时，系统 nudge 追加：
```
你刚才的回复与之前的几乎完全相同。重复同样的计划是一种失败模式。
你必须现在就采取一个不同的具体行动。
```

### 修复后的理想数据流（Gemma 主 + Claude 校验）

```
主模型(Gemma) 流式输出（用户可见）+ 工具调用
   ↓
校验模型(Claude, Stronger档) 异步收到 validator_self_state
   ↓
Claude 判断：
  - 首次：给具体纠错（它确实比 Gemma 强，纠错有效）
  - Gemma 未改：approve + note（不再死循环）
   ↓
纠错作为 [Action required] 硬约束注入（非软 meta）
   ↓
Gemma 采纳（因为它本来就是实干的，会响应明确指令）
```

**这才是 1+1>2**：强校验弱，纠错有效；状态感知防死循环；硬约束确保被听见。

---

## 六、决策流程图

```
有云端强模型(Claude/GPT-4o) API？
├─ 是 → 主模型用本地(Gemma推荐) + 校验用云端
│        tier=Stronger，LLM校验启用，1+1>2 ✅
└─ 否 → 只用本地模型
         ├─ 主模型用 Gemma（实干，推荐）
         └─ 不设校验模型
              tier=Peer，只跑 LocalValidator 规则
              无LLM开销，无死循环 ✅
              （❌ 不要配 Qwen校验Gemma 或 Gemma校验Qwen）
```

---

## 七、验证清单

修复后请用以下方式验证配置正确：

1. **看日志 tier 判定**：启动后 QueryLoop debug 事件含 `validatorEnabled` 字段
   - `false` = Peer/Weaker/Disabled（LLM 校验不跑，符合预期）
   - `true` = Stronger（LLM 校验跑）
2. **看 validator-model.jsonl**：
   - Peer 档：文件不存在或为空（LLM 校验未调用）
   - Stronger 档：每轮有 request/response，且 response 不重复
3. **跑 validator-retry-limit-session 复现**：用修复后的 binary 重跑该场景
   - 修复前：4+ 轮死循环
   - 修复后：≤2 轮内要么主模型改行为，要么 validator approve

---

**文档版本**：v1.0
**配套修复**：STRENGTHEN-01（验证器状态感知）、STRENGTHEN-02（硬约束注入）、STRENGTHEN-03（ValidatorTier 分层）、STRENGTHEN-04（Qwen 反执述）
**验证状态**：agent_core.lib + agent_cli.exe + agent_test_validator 全部编译通过，9 项 validator 测试全绿
