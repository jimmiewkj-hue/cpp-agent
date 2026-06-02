// Test individual tool modules ? aligned with local-ace tools/*/
#include "tools/BashTool.h"
#include "tools/FileReadTool.h"
#include "tools/FileWriteTool.h"
#include "tools/FileEditTool.h"
#include "tools/GlobTool.h"
#include "tools/GrepTool.h"
#include "tools/TodoWriteTool.h"
#include "tools/WebFetchTool.h"
#include "tools/WebSearchTool.h"
#include "tools/NotebookEditTool.h"
#include "tools/TaskCreateTool.h"

#include <cassert>
#include <iostream>
#include <string>

static int failures = 0;
static void Check(bool c, const char* l) { if(!c){std::cerr<<"FAIL: "<<l<<std::endl;++failures;} }

using namespace agent::tools;

void TestBashTool() {
  Check(std::string(kBashToolName) == "Bash", "BashTool name");
  std::string desc = kBashToolDescription;
  Check(desc.find("PowerShell") != std::string::npos, "BashTool mentions PowerShell");
  Check(desc.find("Select-String") != std::string::npos, "BashTool mentions Select-String");
  
  std::string schema = GetBashToolInputSchema();
  Check(schema.find("command") != std::string::npos, "BashTool schema has command");
  Check(schema.find("required") != std::string::npos, "BashTool schema has required");
  
  std::string prompt = BuildBashToolPrompt();
  Check(prompt.find("Bash:") != std::string::npos, "BashTool prompt has name");
}

void TestFileReadTool() {
  Check(std::string(kFileReadToolName) == "Read", "FileReadTool name");
  std::string desc = kFileReadToolDescription;
  Check(desc.find("Reads a file") != std::string::npos, "FileReadTool description");
  
  std::string schema = GetFileReadToolInputSchema();
  Check(schema.find("file_path") != std::string::npos, "FileReadTool schema has file_path");
  Check(schema.find("offset") != std::string::npos, "FileReadTool schema has offset");
  Check(schema.find("limit") != std::string::npos, "FileReadTool schema has limit");
  
  std::string prompt = BuildFileReadToolPrompt();
  Check(prompt.find("Read:") != std::string::npos, "FileReadTool prompt");
}

void TestFileWriteTool() {
  Check(std::string(kFileWriteToolName) == "Write", "FileWriteTool name");
  std::string desc = kFileWriteToolDescription;
  Check(desc.find("overwrite") != std::string::npos || desc.find("Writes") != std::string::npos,
        "FileWriteTool description");
  
  std::string schema = GetFileWriteToolInputSchema();
  Check(schema.find("file_path") != std::string::npos, "FileWriteTool schema has file_path");
  Check(schema.find("content") != std::string::npos, "FileWriteTool schema has content");
}

void TestFileEditTool() {
  Check(std::string(kFileEditToolName) == "Edit", "FileEditTool name");
  std::string schema = GetFileEditToolInputSchema();
  Check(schema.find("old_string") != std::string::npos, "FileEditTool schema has old_string");
  Check(schema.find("new_string") != std::string::npos, "FileEditTool schema has new_string");
}

void TestGlobTool() {
  Check(std::string(kGlobToolName) == "Glob", "GlobTool name");
  std::string schema = GetGlobToolInputSchema();
  Check(schema.find("pattern") != std::string::npos, "GlobTool schema has pattern");
}

void TestGrepTool() {
  Check(std::string(kGrepToolName) == "Grep", "GrepTool name");
  std::string desc = kGrepToolDescription;
  Check(desc.find("ripgrep") != std::string::npos || desc.find("Search") != std::string::npos,
        "GrepTool description");
  std::string schema = GetGrepToolInputSchema();
  Check(schema.find("pattern") != std::string::npos, "GrepTool schema has pattern");
}

void TestTodoWriteTool() {
  Check(std::string(kTodoWriteToolName) == "TodoWrite", "TodoWriteTool name");
  std::string desc = kTodoWriteToolDescription;
  Check(desc.find("task list") != std::string::npos, "TodoWriteTool description");
  std::string schema = GetTodoWriteToolInputSchema();
  Check(schema.find("todos") != std::string::npos, "TodoWriteTool schema has todos");
}

void TestWebFetchTool() {
  Check(std::string(kWebFetchToolName) == "WebFetch", "WebFetchTool name");
  std::string desc = kWebFetchToolDescription;
  Check(desc.find("URL") != std::string::npos || desc.find("url") != std::string::npos, "WebFetchTool description mentions URL");
  Check(desc.find("prompt") != std::string::npos || desc.find("analy") != std::string::npos, "WebFetchTool description mentions prompt/analysis");
  std::string schema = GetWebFetchToolInputSchema();
  Check(schema.find("url") != std::string::npos, "WebFetchTool schema has url");
  Check(schema.find("prompt") != std::string::npos, "WebFetchTool schema has prompt");
  std::string prompt = BuildWebFetchToolPrompt();
  Check(prompt.find("WebFetch") != std::string::npos, "WebFetchTool prompt contains name");
}

void TestWebSearchTool() {
  Check(std::string(kWebSearchToolName) == "WebSearch", "WebSearchTool name");
  std::string desc = kWebSearchToolDescription;
  Check(desc.find("search") != std::string::npos || desc.find("Search") != std::string::npos, "WebSearchTool description mentions search");
  std::string schema = GetWebSearchToolInputSchema();
  Check(schema.find("query") != std::string::npos, "WebSearchTool schema has query");
  std::string prompt = BuildWebSearchToolPrompt();
  Check(prompt.find("WebSearch") != std::string::npos, "WebSearchTool prompt contains name");
}

void TestNotebookEditTool() {
  Check(std::string(kNotebookEditToolName) == "NotebookEdit", "NotebookEditTool name");
  std::string desc = kNotebookEditToolDescription;
  Check(desc.find("notebook") != std::string::npos || desc.find("Jupyter") != std::string::npos, "NotebookEditTool description mentions notebook");
  Check(desc.find("cell") != std::string::npos, "NotebookEditTool description mentions cell");
  std::string schema = GetNotebookEditToolInputSchema();
  Check(schema.find("notebook_path") != std::string::npos, "NotebookEditTool schema has notebook_path");
  Check(schema.find("cell_number") != std::string::npos, "NotebookEditTool schema has cell_number");
  Check(schema.find("new_source") != std::string::npos, "NotebookEditTool schema has new_source");
  std::string prompt = BuildNotebookEditToolPrompt();
  Check(prompt.find("NotebookEdit") != std::string::npos, "NotebookEditTool prompt contains name");
}

void TestTaskCreateTool() {
  Check(std::string(kTaskCreateToolName) == "TaskCreate", "TaskCreateTool name");
  std::string desc = kTaskCreateToolDescription;
  Check(desc.find("subagent") != std::string::npos || desc.find("agent") != std::string::npos, "TaskCreateTool description mentions agent");
  Check(desc.find("task") != std::string::npos || desc.find("Task") != std::string::npos, "TaskCreateTool description mentions task");
  std::string schema = GetTaskCreateToolInputSchema();
  Check(schema.find("task") != std::string::npos, "TaskCreateTool schema has task parameter");
  Check(schema.find("description") != std::string::npos, "TaskCreateTool schema has description parameter");
  std::string prompt = BuildTaskCreateToolPrompt();
  Check(prompt.find("TaskCreate") != std::string::npos, "TaskCreateTool prompt contains name");
}

int main() {
  std::cout << "=== Tool Module Tests ===" << std::endl;
  TestBashTool();
  TestFileReadTool();
  TestFileWriteTool();
  TestFileEditTool();
  TestGlobTool();
  TestGrepTool();
  TestTodoWriteTool();
  TestWebFetchTool();
  TestWebSearchTool();
  TestNotebookEditTool();
  TestTaskCreateTool();
  std::cout << "=== " << failures << " failure(s) ===" << std::endl;
  return failures > 0 ? 1 : 0;
}
