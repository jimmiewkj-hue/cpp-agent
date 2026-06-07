#include "tools/TaskCreateTool.h"
#include <sstream>
namespace agent { namespace tools {
const char* kTaskCreateToolName = "TaskCreate";
const char* kTaskCreateToolDescription =
  "Creates a new task (subagent) to handle complex, multi-step tasks autonomously.\n"
  "- Launches a new agent to handle complex, multi-step tasks autonomously\n"
  "- The task agent will be given a specific objective and can use available tools\n"
  "- The task agent will return one final message; summarize the result as needed\n"
  "- Clearly tell the agent whether it should write code or only perform search/read operations\n"
  "- If an agent is suitable for the user's request, prefer using it proactively\n\n"
  "Usage notes:\n"
  "- Launch multiple agents concurrently whenever possible to maximize performance\n"
  "- Each agent invocation is stateless, so your prompt should describe the task\n"
  "  the subagent needs to accomplish, but should not expand the request into\n"
  "  unnecessary details or steps\n"
  "- The agent will return one final message; summarize the result for the user as needed\n"
  "- Clearly tell the agent whether it should write code or only perform search/read operations\n"
  "- If an agent is suitable for the user's request, prefer using it proactively\n\n"
  "IMPORTANT: When creating tasks that involve code generation or file modification,\n"
  "always instruct the agent to VERIFY its work:\n"
  "- Include 'run the code and verify the output' in the task prompt\n"
  "- Include 'run tests if available' in the task prompt\n"
  "- Include 'check for compilation/import errors' in the task prompt\n"
  "This ensures the sub-agent follows the write-run-verify closed loop.\n\n"
  "Example usage:\n"
  "- task='Write a Python script to process CSV data, then run it to verify it works'\n"
  "- task='Search the codebase for all authentication-related logic'\n"
  "- task='Read and summarize the top 5 files matching a pattern'";
std::string GetTaskCreateToolInputSchema() {
  return "{"
    "\"type\":\"object\","
    "\"properties\":{"
      "\"task\":{\"type\":\"string\",\"description\":\"The task for the subagent to perform\"},"
      "\"description\":{\"type\":\"string\",\"description\":\"Short (3-5 word) description of the task\"},"
      "\"model\":{\"type\":\"string\",\"description\":\"Optional model to use for the subagent\"}"
    "},"
    "\"required\":[\"task\",\"description\"]"
  "}";
}
std::string BuildTaskCreateToolPrompt() {
  std::ostringstream p; p << "- " << kTaskCreateToolName << ": " << kTaskCreateToolDescription; return p.str();
}
}}