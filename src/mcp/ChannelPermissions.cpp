#include "mcp/ChannelPermissions.h"

#include <algorithm>
#include <sstream>

namespace agent {
namespace mcp {

namespace {

bool IsInList(const std::string& name,
              const std::vector<std::string>& list) {
  return std::find(list.begin(), list.end(), name) != list.end();
}

std::string FullyQualifiedName(const std::string& serverName,
                                const std::string& toolName) {
  if (serverName.empty()) return toolName;
  return serverName + "." + toolName;
}

}  // namespace

McpToolPermission CheckMcpToolPermission(
    const McpChannelPermissionsConfig& config,
    const std::string& serverName,
    const std::string& toolName,
    bool readOnlyHint,
    bool destructiveHint) {
  return ResolveEffectivePermission(
      config, serverName, toolName, readOnlyHint, destructiveHint);
}

bool ShouldRequireApproval(
    const McpChannelPermissionsConfig& config,
    const McpToolPermission& permission) {
  if (!config.enabled) return false;
  if (permission.requiresApproval) return true;
  return false;
}

McpToolPermission ResolveEffectivePermission(
    const McpChannelPermissionsConfig& config,
    const std::string& serverName,
    const std::string& toolName,
    bool readOnlyHint,
    bool destructiveHint) {
  McpToolPermission result;
  result.toolName = FullyQualifiedName(serverName, toolName);
  result.readOnly = readOnlyHint;
  result.allow = true;

  if (!config.enabled) {
    result.reason = "Channel permissions disabled (all tools allowed)";
    return result;
  }

  std::string fqn = result.toolName;

  // 1. Check global blocklist first (highest priority)
  if (IsInList(fqn, config.globalBlocklist) ||
      IsInList(toolName, config.globalBlocklist)) {
    result.allow = false;
    result.reason = "Blocked by global blocklist";
    return result;
  }

  // 2. Check server-specific blocklist
  auto blockIt = config.serverToolBlocklists.find(serverName);
  if (blockIt != config.serverToolBlocklists.end()) {
    if (IsInList(fqn, blockIt->second) ||
        IsInList(toolName, blockIt->second)) {
      result.allow = false;
      result.reason = "Blocked by server blocklist for '" + serverName + "'";
      return result;
    }
  }

  // 3. Check global allowlist (if non-empty, only listed tools allowed)
  if (!config.globalAllowlist.empty()) {
    if (!IsInList(fqn, config.globalAllowlist) &&
        !IsInList(toolName, config.globalAllowlist)) {
      result.allow = false;
      result.reason = "Not in global allowlist";
      return result;
    }
  }

  // 4. Check server-specific allowlist
  auto allowIt = config.serverToolAllowlists.find(serverName);
  if (allowIt != config.serverToolAllowlists.end() &&
      !allowIt->second.empty()) {
    if (!IsInList(fqn, allowIt->second) &&
        !IsInList(toolName, allowIt->second)) {
      result.allow = false;
      result.reason = "Not in allowlist for server '" + serverName + "'";
      return result;
    }
  }

  // 5. Determine approval requirement
  if (config.requireApprovalForDestructive && destructiveHint) {
    result.requiresApproval = true;
  }

  if (config.autoApproveReadOnly && readOnlyHint && !destructiveHint) {
    result.requiresApproval = false;
  }

  result.reason = "Allowed by channel permissions";
  return result;
}

std::string FormatMcpPermissionsSummary(
    const McpChannelPermissionsConfig& config) {
  std::ostringstream out;

  if (!config.enabled) {
    out << "[MCP Channel Permissions] DISABLED ? all tools allowed\n";
    return out.str();
  }

  out << "[MCP Channel Permissions] enabled\n";

  if (!config.globalAllowlist.empty()) {
    out << "  Global allowlist (" << config.globalAllowlist.size()
        << " tools):";
    for (const auto& t : config.globalAllowlist) {
      out << " " << t;
    }
    out << "\n";
  }

  if (!config.globalBlocklist.empty()) {
    out << "  Global blocklist (" << config.globalBlocklist.size()
        << " tools):";
    for (const auto& t : config.globalBlocklist) {
      out << " " << t;
    }
    out << "\n";
  }

  for (const auto& [server, tools] : config.serverToolAllowlists) {
    out << "  Server '" << server << "' allowlist (" << tools.size()
        << " tools)\n";
  }

  for (const auto& [server, tools] : config.serverToolBlocklists) {
    out << "  Server '" << server << "' blocklist (" << tools.size()
        << " tools)\n";
  }

  if (config.autoApproveReadOnly) {
    out << "  Auto-approve read-only tools: yes\n";
  }
  if (config.requireApprovalForDestructive) {
    out << "  Require approval for destructive tools: yes\n";
  }

  return out.str();
}

}  // namespace mcp
}  // namespace agent
