// BashTool.h — aligned with local-ace tools/BashTool/
// Provides the Bash tool's name, description prompt, and input schema.
#pragma once

#include <string>

namespace agent {
namespace tools {

// Tool name constant (mirrors local-ace BASH_TOOL_NAME)
extern const char* kBashToolName;

// Tool description for system prompt (mirrors local-ace BashTool/prompt.ts)
// Describes the tool's purpose, usage patterns, and limitations.
extern const char* kBashToolDescription;

// Input schema JSON for the Bash tool (mirrors local-ace input schema)
// Parameters: command (required, string), description (optional, string),
//             timeout (optional, number, default 120000ms)
std::string GetBashToolInputSchema();

// Build the full tool definition for system prompt injection
// Mirrors local-ace getBashToolPrompt()
std::string BuildBashToolPrompt();

}  // namespace tools
}  // namespace agent
