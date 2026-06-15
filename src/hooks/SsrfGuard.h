// STRENGTHEN-T17: SSRF (Server-Side Request Forgery) guard for URL-based
// tools (WebFetch, WebSearch result following, MCP HTTP transports).
// Blocks requests to private/internal/loopback/metadata addresses that
// could leak cloud credentials or reach internal services.
// Aligned with local-ace utils/hooks/ssrfGuard.ts.

#pragma once

#include <string>

namespace agent {
namespace hooks {

enum class SsrfVerdict {
  Allow,
  Block,
};

struct SsrfCheckResult {
  SsrfVerdict verdict = SsrfVerdict::Allow;
  std::string reason;  // populated when verdict == Block
};

// Check a URL for SSRF risk. Blocks:
// - Loopback (127.0.0.0/8, ::1, localhost)
// - Private ranges (10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16, fc00::/7)
// - Link-local (169.254.0.0/16 — includes AWS/GCP/Azure metadata 169.254.169.254)
// - Metadata service aliases (metadata.google.internal)
// - Non-http(s) schemes (file://, gopher://, ftp://, etc.)
// Allows an operator-configured allowlist of domains via env var
// AGENT_SSRF_ALLOW_DOMAINS (comma-separated) for whitelisting internal
// registries that the agent should legitimately reach.
SsrfCheckResult CheckUrlForSsrf(const std::string& url);

// Convenience: returns true if the URL is safe to fetch.
bool IsUrlSafe(const std::string& url);

}  // namespace hooks
}  // namespace agent
