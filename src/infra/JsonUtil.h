#pragma once

// Central JSON type alias using nlohmann/json.
// Include this header instead of directly including nlohmann_json.hpp
// and declaring `using json = nlohmann::json;` in each source file.

#include "third_party/nlohmann_json.hpp"

namespace agent {
namespace infra {

using json = nlohmann::json;

}  // namespace infra
}  // namespace agent

// Convenience alias at global scope for backward compatibility.
// New code should prefer agent::infra::json.
using json = nlohmann::json;
