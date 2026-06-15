# 沙箱实现评估文档（T18）

> **用途**：评估 cpp-agent 当前沙箱实现的成熟度，决定是否需要引入 OS 级沙箱。
> **结论先行**：当前应用层 pattern 过滤 + 已新增的 SSRF 防护（T17）+ 权限引擎熔断，对单用户本地开发场景**足够**。引入 OS 级沙箱的 ROI 低，建议**不引入**，转为"加测试覆盖 + 文档化边界"。

---

## 一、当前沙箱能力盘点

| 防护层 | 实装 | 位置 |
|---|---|---|
| 命令 deny pattern（rm -rf /, format c:, shutdown） | ✅ | `sandbox/SandboxEnforcer.cpp` kDangerousPatterns |
| 网络 prefix deny（curl/wget/ssh/Invoke-WebRequest） | ✅ | `sandbox/SandboxEnforcer.cpp` kNetworkPrefixes |
| Shell 注入启发式 | ✅ | `MatchesShellInjectionPattern` |
| 工作区路径边界 + 遍历检测 | ✅ | `CheckFilePath` / `ContainsTraversalAttempt` |
| 命令 allowlist / blocklist | ✅ | `MatchesAllowlist` / `MatchesBlocklist` |
| 权限引擎熔断（3 连拒/20 累计拒 → 降级手动） | ✅ | `PermissionEngine::DenialTrackingState` |
| **SSRF 防护（私有 IP / 元数据服务 / 非 http scheme）** | ✅ 新增 | `hooks/SsrfGuard.{h,cpp}`（T17）|
| Bash LLM 分类器（复杂命令二次判定） | ✅ 新增 | `BashClassifier::SetClassifierCallback`（T12）|
| OS 级隔离（seccomp / AppContainer / namespaces） | ❌ | 未实装 |

## 二、威胁模型分析

### cpp-agent 的运行上下文
- **单用户本地开发工具**：用户本人在自己的机器上运行，主模型/校验模型都由用户配置
- **工作区隔离需求**：防止 agent 误改工作区外的文件（已有路径边界）
- **网络外发需求**：WebFetch/WebSearch 需访问公网（已有 SSRF 防护阻止内网）

### OS 级沙箱能防什么 pattern-filter 防不住的？
- **DNS rebinding**：域名解析后变内网 IP。但 SSRF 防护已直接按域名拦截 `metadata.google.internal` 等已知别名；动态 DNS rebinding 需运行时二次解析校验，复杂度高
- **命令拼接绕过**（如 `;rm -rf /`）。但 BashClassifier 的 LLM 分类 + pattern deny 双层已覆盖大部分
- **子进程逃逸**。但 `ProcessRunner` 创建的子进程与 agent 同权限，OS 沙箱对等

### 结论
对**单用户本地开发**场景，pattern 过滤 + SSRF 防护 + LLM 分类 + 权限熔断已构成纵深防御。OS 级沙箱的边际收益低于实装成本（Windows AppContainer 配置复杂，Linux seccomp 需先实装 POSIX HTTP 后端，T00 未完成前不可行）。

## 三、建议（替代引入 OS 沙箱）

1. **加强 pattern 测试覆盖**：新增 `tests/unit/test_sandbox_enforcer.cpp`，覆盖危险命令、路径遍历、网络 prefix 的正反例
2. **记录已知限制**：在 README 标注"沙箱为应用层过滤，非 OS 级隔离；不适合在多租户/不可信模型场景使用"
3. **路线图保留**：若未来支持远程/多租户执行，再评估引入 Windows AppContainer 或 Linux namespaces

## 四、不引入 OS 沙箱的理由汇总

| 维度 | 引入 OS 沙箱 | 维持现状 |
|---|---|---|
| 实装成本 | 高（平台相关，POSIX 后端未就绪） | 零 |
| 边际收益 | 低（单用户本地场景威胁有限） | — |
| 维护成本 | 高（平台 API 演进、权限配置易错） | 低 |
| 兼容性 | Windows AppContainer 与某些开发工具冲突 | 无影响 |

**决策：不引入 OS 级沙箱。维持应用层 pattern 过滤 + T17 SSRF 防护 + T12 LLM 分类 + 权限熔断的纵深防御。**
