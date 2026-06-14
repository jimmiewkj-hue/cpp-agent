#include "infra/EnvUtil.h"
#include "infra/StringUtil.h"

#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace agent {
namespace infra {

std::string GetEnvString(const char* name) {
  if (name == nullptr) return {};
#ifdef _WIN32
  char buffer[1024] = {0};
  DWORD len = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
  if (len == 0 || len >= sizeof(buffer)) return {};
  return std::string(buffer, len);
#else
  const char* value = std::getenv(name);
  if (value == nullptr) return {};
  return std::string(value);
#endif
}

std::string GetEnvOrDefault(const char* name, const std::string& fallback) {
  const std::string value = GetEnvString(name);
  return value.empty() ? fallback : value;
}

int GetEnvInt(const char* name, int fallback) {
  const std::string value = GetEnvString(name);
  if (value.empty()) return fallback;
  try {
    return std::stoi(value);
  } catch (...) {
    return fallback;
  }
}

bool GetEnvBool(const char* name, bool fallback) {
  const std::string value = GetEnvString(name);
  if (value.empty()) return fallback;
  return IsTruthyEnvValue(value);
}

double GetEnvDouble(const char* name, double fallback) {
  const std::string value = GetEnvString(name);
  if (value.empty()) return fallback;
  try {
    return std::stod(value);
  } catch (...) {
    return fallback;
  }
}

}  // namespace infra
}  // namespace agent
