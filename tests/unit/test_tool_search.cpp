// P0-03: Comprehensive tests for ToolSearch (aligned with local-ace ToolSearchTool).
// Covers: exact match, keyword search, select: prefix, relevance scoring,
// edge cases, real-world scenarios.

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#define TEST(name) void test_##name()
#define RUN(name) do { std::cout << "  " << #name << "... "; test_##name(); std::cout << "PASSED" << std::endl; } while(0)
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAILED: " << #cond << " at " << __FILE__ << ":" << __LINE__ << std::endl; std::abort(); } } while(0)
#define CHECK_EQ(a, b) do { if ((a) != (b)) { std::cerr << "FAILED: " << #a << " == " << #b << " (" << (a) << " != " << (b) << ") at " << __FILE__ << ":" << __LINE__ << std::endl; std::abort(); } } while(0)

#include "tools/ToolSearch.h"

using namespace agent::tools;

// ============================================================================
// ComputeToolRelevance tests
// ============================================================================

TEST(exact_name_match_gets_highest_score) {
  int score = ComputeToolRelevance("Bash", "Run shell commands", "Bash");
  CHECK(score >= 100);
}

TEST(prefix_match_gets_high_score) {
  int score = ComputeToolRelevance("BashTool", "Run shell commands", "Bash");
  CHECK(score >= 50);
}

TEST(name_contains_query) {
  int score1 = ComputeToolRelevance("FileReadTool", "Read file contents", "Read");
  int score2 = ComputeToolRelevance("OtherTool", "Something else", "Read");
  CHECK(score1 > score2);
}

TEST(description_match_gets_score) {
  int score1 = ComputeToolRelevance("Tool", "Read file contents from disk", "read");
  int score2 = ComputeToolRelevance("Tool", "Write data to network", "read");
  CHECK(score1 > score2);
}

TEST(multi_word_query_scores_each_word) {
  int score1 = ComputeToolRelevance("FileWriteTool", "Write content to file", "write file");
  int score2 = ComputeToolRelevance("NetworkTool", "Send data over network", "write file");
  CHECK(score1 > score2);
}

TEST(case_insensitive_matching) {
  int scoreLower = ComputeToolRelevance("Bash", "Run commands", "bash");
  int scoreUpper = ComputeToolRelevance("Bash", "Run commands", "BASH");
  int scoreMixed = ComputeToolRelevance("Bash", "Run commands", "BaSh");
  CHECK_EQ(scoreLower, scoreUpper);
  CHECK_EQ(scoreUpper, scoreMixed);
}

TEST(no_match_returns_zero) {
  int score = ComputeToolRelevance("Bash", "Run commands", "zzzxxx_nonexistent");
  CHECK_EQ(score, 0);
}

TEST(single_char_word_skipped) {
  int score = ComputeToolRelevance("Tool", "Description", "a");
  CHECK_EQ(score, 0);  // Single char should be skipped
}

// ============================================================================
// FormatToolSearchResults tests
// ============================================================================

TEST(format_empty_results) {
  ToolSearchResult results;
  results.query = "nonexistent";
  results.totalTools = 10;
  
  std::string formatted = FormatToolSearchResults(results);
  CHECK(formatted.find("No tools found") != std::string::npos);
  CHECK(formatted.find("10") != std::string::npos);
}

TEST(format_with_matches) {
  ToolSearchResult results;
  results.query = "file";
  results.totalTools = 10;
  
  ToolSearchMatch m1;
  m1.toolName = "FileRead";
  m1.description = "Read file contents";
  m1.relevanceScore = 90;
  results.matches.push_back(m1);
  
  ToolSearchMatch m2;
  m2.toolName = "FileWrite";
  m2.description = "Write content to file";
  m2.relevanceScore = 80;
  results.matches.push_back(m2);
  
  std::string formatted = FormatToolSearchResults(results);
  CHECK(formatted.find("FileRead") != std::string::npos);
  CHECK(formatted.find("FileWrite") != std::string::npos);
  CHECK(formatted.find("Read file contents") != std::string::npos);
}

TEST(format_with_deferred_tools) {
  ToolSearchResult results;
  results.query = "lsp";
  results.totalDeferredTools = 3;
  
  ToolSearchMatch m;
  m.toolName = "LSPTool";
  m.description = "Language Server Protocol tool";
  m.isDeferred = true;
  results.matches.push_back(m);
  
  std::string formatted = FormatToolSearchResults(results);
  CHECK(formatted.find("[deferred]") != std::string::npos);
  CHECK(formatted.find("deferred tools available") != std::string::npos);
}

// ============================================================================
// SelectDeferredTool tests
// ============================================================================

TEST(select_deferred_tool_returns_false) {
  // In the current implementation, SelectDeferredTool is a stub
  CHECK(!SelectDeferredTool("any_tool"));
}

// ============================================================================
// Real-world scenarios
// ============================================================================

TEST(real_world_ambiguous_tool_search) {
  // Simulates searching for "write" - should find FileWrite, TodoWrite, etc.
  int scoreWrite = ComputeToolRelevance("FileWrite", "Write content to file", "write");
  int scoreTodoWrite = ComputeToolRelevance("TodoWrite", "Write todo list", "write");
  int scoreRead = ComputeToolRelevance("FileRead", "Read file contents", "write");
  
  CHECK(scoreWrite > 0);
  CHECK(scoreTodoWrite > 0);
  CHECK(scoreRead < scoreWrite);
}

TEST(real_world_bash_tool_search) {
  // Searching for "shell" should match Bash
  int scoreShell = ComputeToolRelevance("Bash", "Execute shell commands in a sandboxed environment", "shell");
  int scorePython = ComputeToolRelevance("Python", "Run Python scripts", "shell");
  
  CHECK(scoreShell > scorePython);
}

TEST(real_world_lsp_tool_search) {
  // Searching for "diagnostic" should find LSP tool
  int scoreLsp = ComputeToolRelevance("LSPTool", "Language Server Protocol: diagnostics, formatting, symbols", "diagnostic");
  int scoreBash = ComputeToolRelevance("Bash", "Execute shell commands", "diagnostic");
  
  CHECK(scoreLsp > scoreBash);
  CHECK(scoreBash == 0);  // Bash has nothing to do with diagnostics
}

TEST(real_world_partial_name_match) {
  // User types partial tool name
  int score1 = ComputeToolRelevance("StreamingToolExecutor", "Streaming execution", "stream");
  int score2 = ComputeToolRelevance("TaskCreateTool", "Create a task", "stream");
  
  CHECK(score1 > score2);
}

int main() {
  std::cout << "=== ToolSearch Tests ===" << std::endl;
  
  std::cout << "[Relevance Scoring]" << std::endl;
  RUN(exact_name_match_gets_highest_score);
  RUN(prefix_match_gets_high_score);
  RUN(name_contains_query);
  RUN(description_match_gets_score);
  RUN(multi_word_query_scores_each_word);
  RUN(case_insensitive_matching);
  RUN(no_match_returns_zero);
  RUN(single_char_word_skipped);
  
  std::cout << "[Formatting]" << std::endl;
  RUN(format_empty_results);
  RUN(format_with_matches);
  RUN(format_with_deferred_tools);
  
  std::cout << "[SelectDeferredTool]" << std::endl;
  RUN(select_deferred_tool_returns_false);
  
  std::cout << "[Real-World Scenarios]" << std::endl;
  RUN(real_world_ambiguous_tool_search);
  RUN(real_world_bash_tool_search);
  RUN(real_world_lsp_tool_search);
  RUN(real_world_partial_name_match);
  
  std::cout << "\nAll ToolSearch tests PASSED" << std::endl;
  return 0;
}
