// P0-03: Policy limits tests

#include <cassert>
#include <iostream>
#include <string>

#define TEST(name) void test_##name()
#define RUN(name) do { std::cout << "  " << #name << "... "; test_##name(); std::cout << "PASSED" << std::endl; } while(0)
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAILED: " << #cond << " at " << __FILE__ << ":" << __LINE__ << std::endl; std::abort(); } } while(0)
#define CHECK_EQ(a, b) do { if ((a) != (b)) { std::cerr << "FAILED: " << #a << " == " << #b << " at " << __FILE__ << ":" << __LINE__ << std::endl; std::abort(); } } while(0)

#include "permissions/PolicyLimits.h"
using namespace agent::permissions;

TEST(default_no_restrictions) {
    PolicyLimits limits;
    CHECK(limits.IsBashExecutionAllowed());
    CHECK(limits.IsNetworkAccessAllowed());
    CHECK(limits.IsFileWriteAllowed());
}

TEST(disabled_config_allows_all) {
    PolicyLimitsConfig cfg;
    cfg.enabled = false;
    PolicyLimits limits(cfg);
    limits.AddRestriction({"bash_execution", true, "test", "org"});
    CHECK(limits.IsBashExecutionAllowed());  // disabled config ignores restrictions
}

TEST(restrict_bash_execution) {
    PolicyLimits limits;
    limits.AddRestriction({"bash_execution", true, "Org policy", "organization"});
    CHECK(!limits.IsBashExecutionAllowed());
    CHECK(limits.IsNetworkAccessAllowed());  // other features unaffected
}

TEST(restrict_network_access) {
    PolicyLimits limits;
    limits.AddRestriction({"network_access", true, "No network", "team"});
    CHECK(!limits.IsNetworkAccessAllowed());
}

TEST(restrict_file_write) {
    PolicyLimits limits;
    limits.AddRestriction({"file_write", true, "Read-only workspace", "org"});
    CHECK(!limits.IsFileWriteAllowed());
}

TEST(restrict_subprocess) {
    PolicyLimits limits;
    limits.AddRestriction({"subprocess_spawn", true, "No forking", "org"});
    CHECK(!limits.IsSubprocessAllowed());
}

TEST(remove_restriction) {
    PolicyLimits limits;
    limits.AddRestriction({"bash_execution", true, "test", "org"});
    CHECK(!limits.IsBashExecutionAllowed());
    limits.RemoveRestriction("bash_execution");
    CHECK(limits.IsBashExecutionAllowed());
}

TEST(get_restriction_details) {
    PolicyLimits limits;
    limits.AddRestriction({"network_access", true, "No external access", "organization"});
    auto* r = limits.GetRestriction("network_access");
    CHECK(r != nullptr);
    CHECK_EQ(r->reason, "No external access");
    CHECK_EQ(r->source, "organization");
}

TEST(get_nonexistent_restriction) {
    PolicyLimits limits;
    CHECK(limits.GetRestriction("nonexistent") == nullptr);
}

TEST(multiple_restrictions) {
    PolicyLimits limits;
    limits.AddRestriction({"bash_execution", true, "", ""});
    limits.AddRestriction({"network_access", true, "", ""});
    CHECK(!limits.IsBashExecutionAllowed());
    CHECK(!limits.IsNetworkAccessAllowed());
    CHECK(limits.IsFileWriteAllowed());  // Not restricted
}

TEST(load_default_restrictions) {
    PolicyLimits limits;
    limits.LoadDefaultRestrictions();
    CHECK(limits.IsRestricted("shell_injection"));
    CHECK(limits.IsRestricted("path_traversal"));
    CHECK(limits.IsRestricted("destructive_system_commands"));
    CHECK(limits.IsBashExecutionAllowed());  // bash itself not restricted by default
}

TEST(format_summary) {
    PolicyLimits limits;
    limits.AddRestriction({"network_access", true, "No network", "org"});
    std::string summary = limits.FormatRestrictionsSummary();
    CHECK(summary.find("network_access") != std::string::npos);
    CHECK(summary.find("org") != std::string::npos);
}

int main() {
    std::cout << "=== Policy Limits Tests ===" << std::endl;
    RUN(default_no_restrictions);
    RUN(disabled_config_allows_all);
    RUN(restrict_bash_execution);
    RUN(restrict_network_access);
    RUN(restrict_file_write);
    RUN(restrict_subprocess);
    RUN(remove_restriction);
    RUN(get_restriction_details);
    RUN(get_nonexistent_restriction);
    RUN(multiple_restrictions);
    RUN(load_default_restrictions);
    RUN(format_summary);
    std::cout << "\nAll policy limits tests PASSED" << std::endl;
    return 0;
}
