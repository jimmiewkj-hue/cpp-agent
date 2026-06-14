#pragma once

// Centralized environment variable access for cpp-agent.
// Consolidates GetEnvString/GetEnvOrDefault/GetEnvInt/GetEnvBool that were
// previously duplicated across main.cpp, ModelClient.cpp, QueryLoop.cpp,
// AutoCompact.cpp, and other files.

#include <string>

namespace agent {
namespace infra {

// Read an environment variable as a string. Returns empty string if not set.
std::string GetEnvString(const char* name);

// Read an environment variable with a fallback default value.
std::string GetEnvOrDefault(const char* name, const std::string& fallback);

// Read an environment variable as an integer. Returns fallback if not set
// or not a valid integer.
int GetEnvInt(const char* name, int fallback);

// Read an environment variable as a boolean.
// Truthy values: "1", "true", "TRUE", "yes", "YES", "on", "ON".
// Falsy values: everything else (including unset).
bool GetEnvBool(const char* name, bool fallback = false);

// Read an environment variable as a double. Returns fallback if not set
// or not a valid number.
double GetEnvDouble(const char* name, double fallback);

}  // namespace infra
}  // namespace agent
