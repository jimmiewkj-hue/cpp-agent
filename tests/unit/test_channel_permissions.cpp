// P0-03: Comprehensive tests for MCP channel permissions (aligned with local-ace).
// Covers: allowlist/blocklist, global/server-level, read-only auto-approve,
// destructive tool approval, edge cases, real-world scenarios.

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#define TEST(name) void test_##name()
#define RUN(name) do { std::cout << "  " << #name << "... "; test_##name(); std::cout << "PASSED" << std::endl; } while(0)
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAILED: " << #cond << " at " << __FILE__ << ":" << __LINE__ << std::endl; std::abort(); } } while(0)
#define CHECK_EQ(a, b) do { if ((a) != (b)) { std::cerr << "FAILED: " << #a << " == " << #b << " (" << (a) << " != " << (b) << ") at " << __FILE__ << ":" << __LINE__ << std::endl; std::abort(); } } while(0)

#include "mcp/ChannelPermissions.h"

using namespace agent::mcp;

// ============================================================================
// Basic permission checks
// ============================================================================

TEST(default_config_allows_all) {
  McpChannelPermissionsConfig config;
  auto perm = CheckMcpToolPermission(config, "server1", "tool1", false, false);
  CHECK(perm.allow);
  CHECK(!perm.requiresApproval);
}

TEST(disabled_config_allows_all) {
  McpChannelPermissionsConfig config;
  config.enabled = false;
  auto perm = CheckMcpToolPermission(config, "server1", "dangerous_tool", false, true);
  CHECK(perm.allow);
  CHECK(!perm.requiresApproval);
}

// ============================================================================
// Global blocklist tests
// ============================================================================

TEST(global_blocklist_blocks_tool) {
  McpChannelPermissionsConfig config;
  config.globalBlocklist = {"dangerous_tool", "rm_rf"};
  auto perm = CheckMcpToolPermission(config, "server1", "dangerous_tool", false, true);
  CHECK(!perm.allow);
  CHECK(perm.reason.find("global blocklist") != std::string::npos);
}

TEST(global_blocklist_with_fqn) {
  McpChannelPermissionsConfig config;
  config.globalBlocklist = {"server1.evil_tool"};
  auto perm = CheckMcpToolPermission(config, "server1", "evil_tool", false, false);
  CHECK(!perm.allow);
}

TEST(global_blocklist_does_not_block_other) {
  McpChannelPermissionsConfig config;
  config.globalBlocklist = {"dangerous_tool"};
  auto perm = CheckMcpToolPermission(config, "server1", "safe_tool", false, false);
  CHECK(perm.allow);
}

// ============================================================================
// Global allowlist tests
// ============================================================================

TEST(global_allowlist_only_allows_listed) {
  McpChannelPermissionsConfig config;
  config.globalAllowlist = {"allowed_tool", "read_file"};
  auto perm1 = CheckMcpToolPermission(config, "server1", "allowed_tool", false, false);
  CHECK(perm1.allow);
  
  auto perm2 = CheckMcpToolPermission(config, "server1", "other_tool", false, false);
  CHECK(!perm2.allow);
  CHECK(perm2.reason.find("global allowlist") != std::string::npos);
}

// ============================================================================
// Server-specific blocklist tests
// ============================================================================

TEST(server_blocklist_overrides_global_allow) {
  McpChannelPermissionsConfig config;
  config.globalAllowlist = {"all_tools_allowed"};
  config.serverToolBlocklists["restricted_server"] = {"dangerous_api"};
  
  auto perm = CheckMcpToolPermission(config, "restricted_server", "dangerous_api", false, true);
  CHECK(!perm.allow);
  CHECK(perm.reason.find("restricted_server") != std::string::npos);
}

TEST(server_blocklist_does_not_affect_other_server) {
  McpChannelPermissionsConfig config;
  config.serverToolBlocklists["server_a"] = {"secret_tool"};
  
  auto perm = CheckMcpToolPermission(config, "server_b", "secret_tool", false, false);
  CHECK(perm.allow);
}

// ============================================================================
// Server-specific allowlist tests
// ============================================================================

TEST(server_allowlist_restricts_tools) {
  McpChannelPermissionsConfig config;
  config.serverToolAllowlists["limited_server"] = {"read_only_tool", "list_tool"};
  
  auto perm1 = CheckMcpToolPermission(config, "limited_server", "read_only_tool", true, false);
  CHECK(perm1.allow);
  
  auto perm2 = CheckMcpToolPermission(config, "limited_server", "write_tool", false, true);
  CHECK(!perm2.allow);
}

// ============================================================================
// Read-only / destructive hints
// ============================================================================

TEST(auto_approve_readonly_tool) {
  McpChannelPermissionsConfig config;
  config.autoApproveReadOnly = true;
  auto perm = CheckMcpToolPermission(config, "server1", "read_tool", true, false);
  CHECK(perm.allow);
  CHECK(!perm.requiresApproval);
}

TEST(require_approval_for_destructive) {
  McpChannelPermissionsConfig config;
  config.requireApprovalForDestructive = true;
  auto perm = CheckMcpToolPermission(config, "server1", "write_tool", false, true);
  CHECK(perm.allow);
  CHECK(perm.requiresApproval);
}

TEST(destructive_readonly_not_auto_approved) {
  McpChannelPermissionsConfig config;
  config.autoApproveReadOnly = true;
  config.requireApprovalForDestructive = true;
  // Tool that is BOTH read-only AND destructive is suspicious - require approval
  auto perm = CheckMcpToolPermission(config, "server1", "suspicious_tool", true, true);
  CHECK(perm.allow);
  CHECK(perm.requiresApproval);
}

TEST(safe_readonly_tool_auto_approved) {
  McpChannelPermissionsConfig config;
  config.autoApproveReadOnly = true;
  auto perm = CheckMcpToolPermission(config, "server1", "list_files", true, false);
  CHECK(perm.allow);
  CHECK(!perm.requiresApproval);
}

// ============================================================================
// ShouldRequireApproval tests
// ============================================================================

TEST(should_require_approval_when_permission_says_so) {
  McpChannelPermissionsConfig config;
  McpToolPermission perm;
  perm.allow = true;
  perm.requiresApproval = true;
  CHECK(ShouldRequireApproval(config, perm));
}

TEST(should_not_require_approval_when_permission_says_no) {
  McpChannelPermissionsConfig config;
  McpToolPermission perm;
  perm.allow = true;
  perm.requiresApproval = false;
  CHECK(!ShouldRequireApproval(config, perm));
}

TEST(should_not_require_approval_when_disabled) {
  McpChannelPermissionsConfig config;
  config.enabled = false;
  McpToolPermission perm;
  perm.allow = true;
  perm.requiresApproval = true;
  CHECK(!ShouldRequireApproval(config, perm));
}

// ============================================================================
// FormatMcpPermissionsSummary tests
// ============================================================================

TEST(format_summary_disabled) {
  McpChannelPermissionsConfig config;
  config.enabled = false;
  std::string summary = FormatMcpPermissionsSummary(config);
  CHECK(summary.find("DISABLED") != std::string::npos);
}

TEST(format_summary_enabled_with_lists) {
  McpChannelPermissionsConfig config;
  config.enabled = true;
  config.globalAllowlist = {"tool1", "tool2"};
  config.globalBlocklist = {"evil_tool"};
  config.serverToolAllowlists["server_a"] = {"tool_a"};
  config.serverToolBlocklists["server_b"] = {"tool_b"};
  
  std::string summary = FormatMcpPermissionsSummary(config);
  CHECK(summary.find("Global allowlist") != std::string::npos);
  CHECK(summary.find("Global blocklist") != std::string::npos);
  CHECK(summary.find("server_a") != std::string::npos);
  CHECK(summary.find("server_b") != std::string::npos);
  CHECK(summary.find("Auto-approve read-only") != std::string::npos);
}

// ============================================================================
// Edge cases
// ============================================================================

TEST(empty_tool_name_handled) {
  McpChannelPermissionsConfig config;
  auto perm = CheckMcpToolPermission(config, "server1", "", false, false);
  CHECK(perm.allow);  // Empty tool name should not crash
}

TEST(empty_server_name_handled) {
  McpChannelPermissionsConfig config;
  config.globalBlocklist = {"tool"};
  auto perm = CheckMcpToolPermission(config, "", "tool", false, false);
  CHECK(!perm.allow);  // Should still work with empty server name
}

TEST(blocklist_takes_priority_over_allowlist) {
  McpChannelPermissionsConfig config;
  config.globalAllowlist = {"versatile_tool"};
  config.globalBlocklist = {"versatile_tool"};
  auto perm = CheckMcpToolPermission(config, "server1", "versatile_tool", false, false);
  CHECK(!perm.allow);  // Blocklist wins
}

TEST(duplicate_entries_handled) {
  McpChannelPermissionsConfig config;
  config.globalBlocklist = {"tool", "tool", "tool"};  // Duplicates
  auto perm = CheckMcpToolPermission(config, "server1", "tool", false, false);
  CHECK(!perm.allow);  // Still blocked
}

// ============================================================================
// Real-world scenario: MCP server with mixed tool set
// ============================================================================

TEST(real_world_github_mcp_server) {
  // Simulates a GitHub MCP server with read+write tools
  McpChannelPermissionsConfig config;
  config.globalAllowlist = {
    "github.search_repos", "github.get_file", "github.list_issues",
    "github.create_pr", "github.merge_pr"
  };
  config.serverToolBlocklists["github"] = {"github.delete_repo"};
  config.autoApproveReadOnly = true;
  config.requireApprovalForDestructive = true;
  
  // Read tools should be auto-approved
  auto permSearch = CheckMcpToolPermission(config, "github", "search_repos", true, false);
  CHECK(permSearch.allow);
  CHECK(!permSearch.requiresApproval);
  
  // Write tools should require approval
  auto permCreatePr = CheckMcpToolPermission(config, "github", "create_pr", false, true);
  CHECK(permCreatePr.allow);
  CHECK(permCreatePr.requiresApproval);
  
  // Dangerous tool should be blocked
  auto permDelete = CheckMcpToolPermission(config, "github", "delete_repo", false, true);
  CHECK(!permDelete.allow);
  
  // Tool not in allowlist should be blocked
  auto permUnknown = CheckMcpToolPermission(config, "github", "force_push", false, true);
  CHECK(!permUnknown.allow);
}

int main() {
  std::cout << "=== MCP Channel Permissions Tests ===" << std::endl;
  
  std::cout << "[Basic]" << std::endl;
  RUN(default_config_allows_all);
  RUN(disabled_config_allows_all);
  
  std::cout << "[Global Blocklist]" << std::endl;
  RUN(global_blocklist_blocks_tool);
  RUN(global_blocklist_with_fqn);
  RUN(global_blocklist_does_not_block_other);
  
  std::cout << "[Global Allowlist]" << std::endl;
  RUN(global_allowlist_only_allows_listed);
  
  std::cout << "[Server Blocklist]" << std::endl;
  RUN(server_blocklist_overrides_global_allow);
  RUN(server_blocklist_does_not_affect_other_server);
  
  std::cout << "[Server Allowlist]" << std::endl;
  RUN(server_allowlist_restricts_tools);
  
  std::cout << "[Read-Only/Destructive]" << std::endl;
  RUN(auto_approve_readonly_tool);
  RUN(require_approval_for_destructive);
  RUN(destructive_readonly_not_auto_approved);
  RUN(safe_readonly_tool_auto_approved);
  
  std::cout << "[Approval Logic]" << std::endl;
  RUN(should_require_approval_when_permission_says_so);
  RUN(should_not_require_approval_when_permission_says_no);
  RUN(should_not_require_approval_when_disabled);
  
  std::cout << "[Formatting]" << std::endl;
  RUN(format_summary_disabled);
  RUN(format_summary_enabled_with_lists);
  
  std::cout << "[Edge Cases]" << std::endl;
  RUN(empty_tool_name_handled);
  RUN(empty_server_name_handled);
  RUN(blocklist_takes_priority_over_allowlist);
  RUN(duplicate_entries_handled);
  
  std::cout << "[Real-World Scenario]" << std::endl;
  RUN(real_world_github_mcp_server);
  
  std::cout << "\nAll MCP channel permissions tests PASSED" << std::endl;
  return 0;
}
