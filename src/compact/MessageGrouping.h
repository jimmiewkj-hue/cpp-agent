// MessageGrouping.h — aligned with local-ace services/compact/grouping.ts
// Groups messages at API-round boundaries for safe compaction split points.
#pragma once

#include "core/AgentTypes.h"

#include <string>
#include <vector>

namespace agent {
namespace compact {

// Groups messages by API round: one group per API round-trip.
// A boundary fires when a NEW assistant response begins (different
// message.uuid from the prior assistant). For well-formed conversations
// this is an API-safe split point — the API contract requires every
// tool_use to be resolved before the next assistant turn.
//
// Mirrors local-ace groupMessagesByApiRound.
//
// Replaces human-turn grouping with finer-grained API-round grouping,
// allowing reactive compact to operate on single-prompt agentic sessions.
std::vector<std::vector<core::Message>> GroupMessagesByApiRound(
    const std::vector<core::Message>& messages);

}  // namespace compact
}  // namespace agent
