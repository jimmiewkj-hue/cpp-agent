// SnipProjection.h - aligned with local-ace services/compact/snipProjection.ts
// Currently a no-op pass-through (local-ace isSnipBoundaryMessage always returns false).
#pragma once

#include <vector>

namespace agent {
namespace compact {

// Returns false for all messages - snip projection is not active.
// Mirrors local-ace isSnipBoundaryMessage() which always returns false.
inline bool IsSnipBoundaryMessage() { return false; }

// Identity projection - passes messages through unchanged.
// Mirrors local-ace projectSnippedMessages() identity function.
template <typename T>
std::vector<T> ProjectSnippedMessages(const std::vector<T>& messages) {
  return messages;
}

}  // namespace compact
}  // namespace agent