第二部分：cpp-agent 鲁棒性优化（G:\downloads\claude-code\yuanma-poxi\cpp-agent\src\）
所有改动遵循"增量字段 + 阈值调整 + 新增熔断分支，不改动现有正常路径"原则。

2.1 P0-2 零推理工具调用循环硬熔断（核心）
问题：日志 21 次 textEvents=0 toolEvents=1，模型只发工具调用零推理，agent 反复 ForcedContinuation（19 次）。 根因：textEvents/toolEvents 计数只在 ModelClient.cpp 局部变量里，从不传到 QueryLoopInternalState；ShouldTerminateOnDuplicates 只在 RunTools 阶段跑，不覆盖 no-tool 路径。 改法：

QueryLoop.h：在 QueryLoopInternalState 增加字段 int lastTurnTextChars = 0;（本轮回推理文本字符数）、int consecutiveToolOnlyTurns = 0;（连续纯工具零推理轮数）。
QueryLoop.cpp ApplyStepModelCall 的 SSE 回调：累计 text_delta 字符数到局部变量，流结束后写入 state.lastTurnTextChars；若 state.toolUseBlocks 非空且 lastTurnTextChars < kMinReasoningChars（Gemma 族用 20，其它 10），++state.consecutiveToolOnlyTurns，否则清零。
HandleNoToolContinuation 入口处新增熔断检查：consecutiveToolOnlyTurns >= GetModelFamilyPolicy().maxToolOnlyTurns（Gemma=4，Qwen=3，其它=2）→ terminalReason="tool_only_loop"，completed=true，return。这复用现有 terminalReason 自由字符串模式，无需新枚举。
2.2 P0-1 上下文膨胀收敛（按消息数触发 compact）
问题：上下文 1→160 不压缩，GetContextWindowForFamily 对所有族返回 200000，autocompact 阈值 167000 token 不可达。 根因：压缩只看 token 估算（对中文短消息严重低估），消息数仅作下限保护不作触发。 改法（最小侵入，只动阈值判定）：

QueryLoop.h ModelFamilyPolicy 增加字段 int compactMessageThreshold = 0;（0=禁用消息数触发）。
AgentTypes.h GetModelFamilyPolicy：Gemma 设 compactMessageThreshold=80，Qwen=60，MiMo=50，Claude/Generic=0（云端模型保留原 token 逻辑）。
QueryLoop.cpp ApplyStepAutocompact：在现有 token 阈值判定（:2735）之前增加消息数触发——state.messagesForTurn.size() >= policy.compactMessageThreshold && policy.compactMessageThreshold > 0 → 直接进入压缩流程（跳过 token 阈值的 return false）。同样在 ApplyStepCollapse（:2675）增加消息数触发。
这是加法：原有 token 阈值路径完全保留，只是多一条"消息数超限也触发"的并行路径。
2.3 P0-4 运行验证闭环：Bash 失败也计入未验证
问题：PostToolTurnProcessing 里 hadBashRun 为真就清零 consecutiveWriteWithoutVerifyCount，即使 Bash 失败（exit≠0）也算"已验证"，模型不会重试修复。 根因：验证计数器只看"有没有跑 Bash"，不看"跑成功没"。 改法（QueryLoop.cpp:1610-1623）：

扫描时同时记录最近 Bash 工具结果的 isError 标志（block.asToolResult.isError）。
hadBashRun && !bashFailed → 清零计数器（真验证通过）。
hadBashRun && bashFailed → 不清零，且若输出含 traceback/error，++consecutiveWriteWithoutVerifyCount 并保留 nudge 预算，驱动模型按错误修复后重跑。
这样验证 nudge 不会因一次失败 Bash 而耗尽，闭环真正合上。
2.4 P1 长轮次退化：maxOutputTokens 阶梯 + 上下文超限主动 compact
问题：单轮耗时 4s→115s，长上下文量化模型退化。 改法（轻量，复用 2.2）：

2.2 的消息数 compact 已能在 80 条时主动压缩，直接缓解长轮次退化（主因是上下文过长）。
额外：HandleMaxOutputTokens 的静默截断启发（:3379-3389）依赖 forcedContinuationCount >= 3，但该计数器从未自增（探索发现的潜在 bug）。在 HandleNoToolContinuation 和 ContinueWithFollowup 产生 ForcedContinuation 时 ++state.forcedContinuationCount，让截断检测真正生效（这是修复已有但失效的逻辑，非新功能）。
第三部分：验证方式
测试项目：改完后用 python verify_analysis.py 跑通（签名已对齐），检查 output/relationship_summary.md 不再全是"师徒"；python main.py 不再 TypeError（需 Flask/networkx/matplotlib，已有 requirements.txt）。
cpp-agent：改完后用 cmake --build build 编译通过（不引入新依赖，仅 .h/.cpp 内增量）；重跑 jianlai_graph1 观察日志：tool_only_loop 终止出现、contextMessages 在 ~80 触发 compact 不再涨到 160、单轮耗时回落。
不做的事
不重写 cpp-agent 架构（已判定架构完善）。
不动 compact/ 目录死代码（与运行路径无关，清理属于另一次重构）。
不改 GetContextWindowForFamily 的 200000 返回值（会影响云端 Claude 的行为，且 2.2 的消息数触发已针对性解决本地模型问题，更安全）。
不自动执行入口脚本（保持 agent 不越权代跑，靠 2.3 闭环驱动模型自跑）。