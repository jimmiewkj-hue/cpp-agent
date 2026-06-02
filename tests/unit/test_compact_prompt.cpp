// Test CompactPrompt — aligned with local-ace compact/prompt.ts
#include "compact/CompactPrompt.h"

#include <cassert>
#include <iostream>
#include <string>

static int failures = 0;

static void Check(bool condition, const char* label) {
  if (!condition) {
    std::cerr << "FAIL: " << label << std::endl;
    ++failures;
  }
}

void TestNoToolsPreamble() {
  std::string preamble = agent::compact::BuildNoToolsPreamble();
  Check(!preamble.empty(), "NoToolsPreamble is non-empty");
  Check(preamble.find("CRITICAL: Respond with TEXT ONLY") != std::string::npos,
        "NoToolsPreamble contains CRITICAL directive");
  Check(preamble.find("Do NOT use Read, Bash") != std::string::npos,
        "NoToolsPreamble lists forbidden tools");
  Check(preamble.find("REJECTED") != std::string::npos,
        "NoToolsPreamble warns about rejection");
}

void TestBuildCompactPrompt() {
  // Base prompt without custom instructions
  std::string prompt = agent::compact::BuildCompactPrompt("");
  Check(!prompt.empty(), "BuildCompactPrompt is non-empty");
  Check(prompt.find("CRITICAL: Respond with TEXT ONLY") != std::string::npos,
        "Base prompt starts with NO_TOOLS_PREAMBLE");
  Check(prompt.find("Your task is to create a detailed summary") != std::string::npos,
        "Base prompt contains task description");
  Check(prompt.find("1. Primary Request and Intent") != std::string::npos,
        "Base prompt has section 1");
  Check(prompt.find("9. Optional Next Step") != std::string::npos,
        "Base prompt has all 9 sections");
  Check(prompt.find("<analysis>") != std::string::npos,
        "Base prompt has example structure");
  Check(prompt.find("REMINDER: Do NOT call any tools") != std::string::npos,
        "Base prompt ends with NO_TOOLS_TRAILER");
  
  // With custom instructions
  std::string custom = "Focus on TypeScript code changes and test output.";
  prompt = agent::compact::BuildCompactPrompt(custom);
  Check(prompt.find("Additional Instructions:") != std::string::npos,
        "Custom instructions section present");
  Check(prompt.find(custom) != std::string::npos,
        "Custom instructions content present");
}

void TestBuildPartialCompactPrompt() {
  // "From" direction (default)
  std::string prompt = agent::compact::BuildPartialCompactPrompt("");
  Check(!prompt.empty(), "Partial compact prompt is non-empty");
  Check(prompt.find("RECENT portion of the conversation") != std::string::npos,
        "Partial prompt mentions RECENT scope");
  Check(prompt.find("follow earlier retained context") != std::string::npos,
        "Partial prompt explains retained context");
  Check(prompt.find("1. Primary Request and Intent") != std::string::npos,
        "Partial prompt has numbered sections");
  
  // "UpTo" direction
  prompt = agent::compact::BuildPartialCompactPrompt(
      "", agent::compact::PartialCompactDirection::UpTo);
  Check(prompt.find("placed at the start of a continuing session") != std::string::npos,
        "UpTo prompt mentions continuing session");
  Check(prompt.find("Context for Continuing Work") != std::string::npos,
        "UpTo prompt has Context for Continuing Work section");
  Check(prompt.find("Work Completed") != std::string::npos,
        "UpTo prompt has Work Completed section");
}

void TestDetailedAnalysisInstruction() {
  // Base instruction should mention "the conversation"
  std::string base = agent::compact::kDetailedAnalysisInstructionBase;
  Check(base.find("the conversation") != std::string::npos,
        "Base instruction scopes to 'the conversation'");
  Check(base.find("Chronologically analyze each message") != std::string::npos,
        "Base instruction mentions chronological analysis");
  Check(base.find("file names") != std::string::npos,
        "Base instruction lists file names");
  Check(base.find("Errors that you ran into") != std::string::npos,
        "Base instruction mentions errors");
  
  // Partial instruction should mention "recent messages"
  std::string partial = agent::compact::kDetailedAnalysisInstructionPartial;
  Check(partial.find("recent messages") != std::string::npos,
        "Partial instruction scopes to 'recent messages'");
}

void TestTemplateExpansion() {
  // The BASE_COMPACT_PROMPT contains ${DETAILED_ANALYSIS_INSTRUCTION_BASE}
  // After expansion, the placeholder should be replaced with actual text
  std::string prompt = agent::compact::BuildCompactPrompt("");
  // Placeholder should NOT appear
  Check(prompt.find("${DETAILED_ANALYSIS_INSTRUCTION_BASE}") == std::string::npos,
        "Template placeholder replaced in base prompt");
  
  // The PARTIAL_COMPACT_PROMPT contains ${DETAILED_ANALYSIS_INSTRUCTION_PARTIAL}
  std::string partial = agent::compact::BuildPartialCompactPrompt("");
  Check(partial.find("${DETAILED_ANALYSIS_INSTRUCTION_PARTIAL}") == std::string::npos,
        "Template placeholder replaced in partial prompt");
}

int main() {
  std::cout << "=== CompactPrompt Tests ===" << std::endl;
  
  TestNoToolsPreamble();
  TestBuildCompactPrompt();
  TestBuildPartialCompactPrompt();
  TestDetailedAnalysisInstruction();
  TestTemplateExpansion();
  
  std::cout << "=== " << failures << " failure(s) ===" << std::endl;
  return failures > 0 ? 1 : 0;
}
