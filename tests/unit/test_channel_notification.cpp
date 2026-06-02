#include <cassert>
#include <iostream>
#include <string>

#define TEST(name) void test_##name()
#define RUN(name) do { std::cout << "  " << #name << "... "; test_##name(); std::cout << "PASSED" << std::endl; } while(0)
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAILED: " << #cond << "\n"; std::abort(); } } while(0)
#define CHECK_EQ(a, b) do { if ((a) != (b)) { std::cerr << "FAILED: " << #a << " == " << #b << "\n"; std::abort(); } } while(0)

#include "mcp/ChannelNotification.h"
using namespace agent::mcp;

TEST(disabled_by_default) {
    ChannelNotificationRelay relay;
    CHECK(!relay.IsEnabled());
}

TEST(enabled_with_channels) {
    ChannelNotificationConfig cfg;
    cfg.enabled = true;
    cfg.activeChannels = {"telegram"};
    ChannelNotificationRelay relay(cfg);
    CHECK(relay.IsEnabled());
}

TEST(send_permission_request) {
    ChannelNotificationConfig cfg;
    cfg.enabled = true;
    cfg.activeChannels = {"telegram"};
    ChannelNotificationRelay relay(cfg);
    
    std::string id = relay.SendPermissionRequest("Bash", "server1", "Allow Bash execution?");
    CHECK(!id.empty());
    CHECK(relay.HasPendingRequests());
    CHECK_EQ(relay.PendingRequestCount(), 1);
}

TEST(send_disabled_returns_empty) {
    ChannelNotificationRelay relay;
    std::string id = relay.SendPermissionRequest("Bash", "server1", "Allow?");
    CHECK(id.empty());
}

TEST(handle_incoming_response) {
    ChannelNotificationConfig cfg;
    cfg.enabled = true;
    cfg.activeChannels = {"telegram"};
    ChannelNotificationRelay relay(cfg);
    
    std::string id = relay.SendPermissionRequest("Bash", "server1", "Allow?");
    
    bool handlerCalled = false;
    relay.OnResponse(id, [&](const ChannelPermissionResponse& resp) {
        handlerCalled = true;
        CHECK_EQ(resp.behavior, ChannelPermissionResponse::Allow);
    });
    
    ChannelPermissionResponse resp;
    resp.requestId = id;
    resp.behavior = ChannelPermissionResponse::Allow;
    resp.fromServer = "telegram";
    
    CHECK(relay.HandleIncomingResponse(resp));
    CHECK(handlerCalled);
}

TEST(handle_unknown_response) {
    ChannelNotificationRelay relay;
    ChannelPermissionResponse resp;
    resp.requestId = "unknown";
    CHECK(!relay.HandleIncomingResponse(resp));
}

TEST(cancel_all_pending) {
    ChannelNotificationConfig cfg;
    cfg.enabled = true;
    cfg.activeChannels = {"telegram"};
    ChannelNotificationRelay relay(cfg);
    relay.SendPermissionRequest("Bash", "srv", "?");
    relay.SendPermissionRequest("Read", "srv", "?");
    relay.CancelAllPending();
    CHECK(!relay.HasPendingRequests());
    CHECK_EQ(relay.PendingRequestCount(), 0);
}

TEST(unsubscribe_cleans_up) {
    ChannelNotificationConfig cfg;
    cfg.enabled = true;
    cfg.activeChannels = {"telegram"};
    ChannelNotificationRelay relay(cfg);
    std::string id = relay.SendPermissionRequest("Bash", "srv", "?");
    auto unsub = relay.OnResponse(id, [](const auto&) {});
    unsub();
    CHECK(!relay.HasPendingRequests());
}

TEST(send_elicitation_request) {
    ChannelNotificationConfig cfg;
    cfg.enabled = true;
    cfg.activeChannels = {"discord"};
    ChannelNotificationRelay relay(cfg);
    std::string id = relay.SendElicitationRequest("srv", "Input needed", "{}");
    CHECK(!id.empty());
}

int main() {
    std::cout << "=== Channel Notification Tests ===" << std::endl;
    RUN(disabled_by_default);
    RUN(enabled_with_channels);
    RUN(send_permission_request);
    RUN(send_disabled_returns_empty);
    RUN(handle_incoming_response);
    RUN(handle_unknown_response);
    RUN(cancel_all_pending);
    RUN(unsubscribe_cleans_up);
    RUN(send_elicitation_request);
    std::cout << "\nAll channel notification tests PASSED" << std::endl;
    return 0;
}
