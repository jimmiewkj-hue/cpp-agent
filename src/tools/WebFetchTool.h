// WebFetchTool.h - aligned with local-ace tools/WebFetchTool/
#pragma once
#include <string>
namespace agent { namespace tools {
extern const char* kWebFetchToolName;
extern const char* kWebFetchToolDescription;
std::string GetWebFetchToolInputSchema();
std::string BuildWebFetchToolPrompt();
}}