// WebSearchTool.h - aligned with local-ace tools/WebSearchTool/
#pragma once
#include <string>
namespace agent { namespace tools {
extern const char* kWebSearchToolName;
extern const char* kWebSearchToolDescription;
std::string GetWebSearchToolInputSchema();
std::string BuildWebSearchToolPrompt();
}}