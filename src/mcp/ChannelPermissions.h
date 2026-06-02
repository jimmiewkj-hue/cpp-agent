#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace agent {
namespace mcp {

// P0-03: MCP channel permissions (aligned with local-ace channelPermissions).
// Controls which MCP tools are accessible based on the active channel
// (e.g., CLI vs. background automation vs. IDE integration).

struct McpToolPermission {
  std::string toolName;           // Fully qualified name (serverName.toolName)
  bool allow = true;              // Overall allow/deny
  bool requiresApproval = false;  // Needs user approval before execution
  bool readOnly = false;          // Declared as read-only
  std::string reason;             // Human-readable reason for the decision
};

struct McpChannelPermissionsConfig {
  bool enabled = true;
  // Per-server tool allowlists (if empty, all tools allowed for that server)
  std::map<std::string, std::vector<std::string>> serverToolAllowlists;
  // Per-server tool blocklists (always blocked regardless)
  std::map<std::string, std::vector<std::string>> serverToolBlocklists;
  // Global allowlist (applies to all servers)
  std::vector<std::string> globalAllowlist;
  // Global blocklist (applies to all servers)
  std::vector<std::string> globalBlocklist;
  // Auto-approve tools that are marked read-only
  bool autoApproveReadOnly = true;
  // Require approval for destructive tools
  bool requireApprovalForDestructive = true;
};

// Check if a specific MCP tool is permitted given the channel config.
// Returns a permission decision with allow/deny and approval requirements.
McpToolPermission CheckMcpToolPermission(
    const McpChannelPermissionsConfig& config,
    const std::string& serverName,
    const std::string& toolName,
    bool readOnlyHint,
    bool destructiveHint);

// Check if tool should require user approval before execution.
bool ShouldRequireApproval(
    const McpChannelPermissionsConfig& config,
    const McpToolPermission& permission);

// Resolve the effective permission after combining server-level and
// global-level allowlists/blocklists. Blocklists take priority.
McpToolPermission ResolveEffectivePermission(
    const McpChannelPermissionsConfig& config,
    const std::string& serverName,
    const std::string& toolName,
    bool readOnlyHint,
    bool destructiveHint);

// Build a summary of channel permissions for display/debugging.
std::string FormatMcpPermissionsSummary(
    const McpChannelPermissionsConfig& config);

}  // namespace mcp
}  // namespace agent
