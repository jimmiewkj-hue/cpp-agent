#include "core/StateTypes.h"

namespace agent {
namespace core {

AgentConfig AgentConfig::FromDefaults() {
  AgentConfig config;
  config.memoryRoot = "~/.agent/memory";
  config.sessionDir = "~/.agent/sessions";
  config.logDir = "~/.agent/logs";
  return config;
}

}  // namespace core
}  // namespace agent
