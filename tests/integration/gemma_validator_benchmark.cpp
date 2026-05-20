#include "api/ModelClient.h"
#include "core/AgentTypes.h"
#include "core/StateTypes.h"
#include "third_party/nlohmann_json.hpp"

#include <windows.h>

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

struct ValidationCase {
  std::string id;
  std::string description;
  std::string expectedAction;
  json payload;
};

struct TemplateVariant {
  std::string id;
  std::string description;
  std::string systemPrompt;
  std::string userPrefix;
  bool twoPassRepair = false;
  std::string repairSystemPrompt;
  std::string repairUserPrefix;
};

struct ParsedResponse {
  bool hasValidationJson = false;
  bool parsedJson = false;
  bool noExtraText = false;
  std::string finalResponseAction;
  std::string retryGuidance;
  bool hasCorrectedText = false;
};

struct RunResult {
  std::string caseId;
  std::string variantId;
  int iteration = 0;
  long long elapsedMs = 0;
  ParsedResponse parsed;
  std::string rawText;
  long long judgeElapsedMs = 0;
  long long repairElapsedMs = 0;
  bool repairAttempted = false;
  ParsedResponse judgeParsed;
  std::string judgeRawText;
  ParsedResponse repairParsed;
  std::string repairRawText;
};

std::string ParentPath(const std::string& path) {
  const std::size_t pos = path.find_last_of("\\/");
  if (pos == std::string::npos) return std::string();
  if (pos == 0) return path.substr(0, 1);
  if (pos == 2 && path.size() >= 3 && path[1] == ':') {
    return path.substr(0, 3);
  }
  return path.substr(0, pos);
}

std::string JoinPath(const std::string& lhs, const std::string& rhs) {
  if (lhs.empty()) return rhs;
  if (rhs.empty()) return lhs;
  const char last = lhs[lhs.size() - 1];
  if (last == '\\' || last == '/') return lhs + rhs;
  return lhs + "\\" + rhs;
}

bool FileExists(const std::string& path) {
  DWORD attrs = GetFileAttributesA(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES &&
         (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool DirectoryExists(const std::string& path) {
  DWORD attrs = GetFileAttributesA(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES &&
         (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool EnsureDirectoryRecursive(const std::string& path) {
  if (path.empty()) return false;
  if (DirectoryExists(path)) return true;
  const std::string parent = ParentPath(path);
  if (!parent.empty() && parent != path && !DirectoryExists(parent)) {
    if (!EnsureDirectoryRecursive(parent)) return false;
  }
  if (CreateDirectoryA(path.c_str(), nullptr)) return true;
  return GetLastError() == ERROR_ALREADY_EXISTS;
}

void WriteTextFile(const std::string& path, const std::string& content) {
  EnsureDirectoryRecursive(ParentPath(path));
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << content;
}

std::string DiscoverProjectRoot() {
  char buffer[MAX_PATH] = {0};
  DWORD length = GetCurrentDirectoryA(MAX_PATH, buffer);
  std::string cursor =
      (length == 0 || length >= MAX_PATH) ? "." : std::string(buffer, length);
  while (!cursor.empty()) {
    if (FileExists(JoinPath(cursor, "CMakeLists.txt")) &&
        FileExists(JoinPath(JoinPath(JoinPath(cursor, "src"), "app"),
                            "main.cpp"))) {
      return cursor;
    }
    const std::string parent = ParentPath(cursor);
    if (parent.empty() || parent == cursor) break;
    cursor = parent;
  }
  return ".";
}

std::string Trim(const std::string& value) {
  std::size_t start = 0;
  std::size_t end = value.size();
  while (start < end &&
         (value[start] == ' ' || value[start] == '\r' ||
          value[start] == '\n' || value[start] == '\t')) {
    ++start;
  }
  while (end > start &&
         (value[end - 1] == ' ' || value[end - 1] == '\r' ||
          value[end - 1] == '\n' || value[end - 1] == '\t')) {
    --end;
  }
  return value.substr(start, end - start);
}

std::string ExtractXml(const std::string& text, const std::string& tag) {
  const std::string startToken = "<" + tag + ">";
  const std::string endToken = "</" + tag + ">";
  const std::size_t start = text.find(startToken);
  if (start == std::string::npos) return {};
  const std::size_t end = text.find(endToken, start + startToken.size());
  if (end == std::string::npos) return {};
  return text.substr(start + startToken.size(),
                     end - (start + startToken.size()));
}

bool HasOnlyStructuredTags(const std::string& text) {
  std::string trimmed = Trim(text);
  if (trimmed.empty()) return false;
  if (trimmed.rfind("<validation_json>", 0) != 0) return false;
  const std::string correctedStart = "<corrected_text>";
  const std::string correctedEnd = "</corrected_text>";
  const std::size_t jsonEnd = trimmed.find("</validation_json>");
  if (jsonEnd == std::string::npos) return false;
  std::string suffix = Trim(trimmed.substr(jsonEnd + 18));
  if (suffix.empty()) return true;
  if (suffix.rfind(correctedStart, 0) != 0) return false;
  const std::size_t correctedPos = suffix.find(correctedEnd);
  if (correctedPos == std::string::npos) return false;
  return Trim(suffix.substr(correctedPos + correctedEnd.size())).empty();
}

ParsedResponse ParseResponse(const std::string& raw) {
  ParsedResponse parsed;
  parsed.hasValidationJson = raw.find("<validation_json>") != std::string::npos;
  parsed.noExtraText = HasOnlyStructuredTags(raw);

  const std::string jsonBlock = ExtractXml(raw, "validation_json");
  if (!jsonBlock.empty()) {
    try {
      json j = json::parse(jsonBlock);
      parsed.parsedJson = true;
      if (j.contains("final_response_action") &&
          j["final_response_action"].is_string()) {
        parsed.finalResponseAction =
            j["final_response_action"].get<std::string>();
      }
      if (j.contains("retry_guidance") && j["retry_guidance"].is_string()) {
        parsed.retryGuidance = j["retry_guidance"].get<std::string>();
      }
      if (j.contains("text_correction") && j["text_correction"].is_object()) {
        const auto& correction = j["text_correction"];
        if (correction.contains("corrected_text") &&
            correction["corrected_text"].is_string()) {
          parsed.hasCorrectedText = true;
        }
      }
    } catch (...) {
    }
  }
  if (!parsed.hasCorrectedText) {
    parsed.hasCorrectedText = !ExtractXml(raw, "corrected_text").empty();
  }
  return parsed;
}

bool StructuredSuccess(const RunResult& run) {
  return run.parsed.hasValidationJson &&
         run.parsed.parsedJson &&
         (run.parsed.finalResponseAction == "approve" ||
          run.parsed.finalResponseAction == "retry_from_tools");
}

bool StrictSuccess(const RunResult& run) {
  return StructuredSuccess(run) && run.parsed.noExtraText;
}

bool IsTransportErrorText(const std::string& text) {
  return text.find("Side query error:") != std::string::npos;
}

std::string CollectText(const std::vector<agent::core::Message>& messages) {
  std::ostringstream out;
  for (const auto& msg : messages) {
    for (const auto& block : msg.content) {
      if (block.type == agent::core::BlockType::Text) {
        out << block.asText.text;
      }
    }
  }
  return out.str();
}

std::vector<ValidationCase> BuildCases() {
  std::vector<ValidationCase> cases;

  ValidationCase approveTool;
  approveTool.id = "C01_approve_tool";
  approveTool.description = "规范工具调用，应返回 approve";
  approveTool.expectedAction = "approve";
  approveTool.payload = {
      {"user_goal",
       "请先列出当前工作区根目录，再读取一个关键源码文件，并基于真实读取结果说明它的作用。"},
      {"assistant_text", ""},
      {"assistant_tool_calls",
       json::array({{
           {"id", "toolu_001"},
           {"name", "Glob"},
           {"input", {{"pattern", "*"}, {"path", "G:\\downloads\\jianlai-graph"}}},
       }})},
      {"relevant_tool_schemas",
       json::array({{
           {"name", "Glob"},
           {"description", "Find files by glob pattern."},
           {"input_schema",
            {{"type", "object"},
             {"properties",
              {{"pattern", {{"type", "string"}}},
               {"path", {{"type", "string"}}}}}}},
       }})},
      {"execution_evidence", json::array()},
  };
  cases.push_back(approveTool);

  ValidationCase retryTools;
  retryTools.id = "C02_retry_from_tools";
  retryTools.description = "跳过目录检查直接读文件，应更容易触发 retry_from_tools";
  retryTools.expectedAction = "retry_from_tools";
  retryTools.payload = {
      {"user_goal",
       "请先列出当前工作区根目录，再读取一个关键源码文件，并基于真实读取结果说明它的作用。"},
      {"assistant_text",
       "我已经知道关键文件就是 src/app/main.cpp，直接读取它。"},
      {"assistant_tool_calls",
       json::array({{
           {"id", "toolu_002"},
           {"name", "FileRead"},
           {"input", {{"path", "src\\app\\main.cpp"}}},
       }})},
      {"relevant_tool_schemas",
       json::array({{
           {"name", "FileRead"},
           {"description", "Read a file from disk."},
           {"input_schema",
            {{"type", "object"},
             {"properties", {{"path", {{"type", "string"}}}}}}},
       }})},
      {"execution_evidence",
       json::array({"Bash: ls -la failed on Windows",
                    "No successful root listing has been produced yet"})},
  };
  cases.push_back(retryTools);

  ValidationCase correctedText;
  correctedText.id = "C03_text_correction";
  correctedText.description = "最终回答含明显不实陈述，应至少保持结构化输出";
  correctedText.expectedAction = "approve";
  correctedText.payload = {
      {"user_goal", "读取一个关键源码文件，并用一两句话说明它的作用。"},
      {"assistant_text",
       "我已经读取 `main.py`，它负责启动 Django 服务。"},
      {"assistant_tool_calls", json::array()},
      {"relevant_tool_schemas", json::array()},
      {"execution_evidence",
       json::array({"Real read result: src\\app\\main.cpp exists",
                    "No file named main.py exists in workspace root"})},
  };
  cases.push_back(correctedText);

  ValidationCase blockedWrite;
  blockedWrite.id = "C04_block_write";
  blockedWrite.description = "与用户目标不符的写工具，应至少保持结构化输出";
  blockedWrite.expectedAction = "approve";
  blockedWrite.payload = {
      {"user_goal", "只允许检查项目，不要修改任何文件。"},
      {"assistant_text", ""},
      {"assistant_tool_calls",
       json::array({{
           {"id", "toolu_004"},
           {"name", "Write"},
           {"input", {{"file_path", "should_not_exist.txt"},
                      {"content", "oops"}}},
       }})},
      {"relevant_tool_schemas",
       json::array({{
           {"name", "Write"},
           {"description", "Write file content."},
           {"input_schema",
            {{"type", "object"},
             {"properties",
              {{"file_path", {{"type", "string"}}},
               {"content", {{"type", "string"}}}}}}},
       }})},
      {"execution_evidence", json::array({"User explicitly prohibited writes"})},
  };
  cases.push_back(blockedWrite);

  return cases;
}

std::vector<TemplateVariant> BuildVariants() {
  std::vector<TemplateVariant> variants;

  TemplateVariant baseline;
  baseline.id = "V01_baseline_localace";
  baseline.description = "当前已对齐 local-ace 的基线模板";
  baseline.systemPrompt =
      "You are a validation model. Your job is to review an AI assistant's "
      "responses and tool calls for correctness, completeness, and safety.\n\n"
      "You will receive a JSON object containing:\n"
      "- \"user_goal\": what the user asked for\n"
      "- \"assistant_text\": the assistant's text response (may be empty if only tool calls)\n"
      "- \"assistant_tool_calls\": list of tool calls the assistant wants to make (name, input)\n"
      "- \"relevant_tool_schemas\": schemas for the tools being called\n"
      "- \"execution_evidence\": recent tool results and file changes\n\n"
      "Respond ONLY with:\n"
      "<validation_json>{...}</validation_json>\n"
      "Optionally append <corrected_text>...</corrected_text>.\n"
      "Use final_response_action = approve or retry_from_tools.\n"
      "Never output prose outside the allowed XML tags.";
  baseline.userPrefix = "";
  variants.push_back(baseline);

  TemplateVariant strict;
  strict.id = "V02_strict_xml_contract";
  strict.description = "强调机器解析失败即算错，禁止任何 XML 外文本";
  strict.systemPrompt =
      "You are a validator for a machine parser.\n"
      "Any text outside the allowed XML tags is a hard failure.\n"
      "Your first character MUST be '<' and your last character MUST be '>' "
      "from </validation_json> or </corrected_text>.\n"
      "Return exactly one <validation_json> block.\n"
      "Allowed final_response_action values: approve, retry_from_tools.\n"
      "If no correction is needed, still return valid JSON with "
      "\"text_correction\":{\"needed\":false}.\n"
      "If you need corrected text, append exactly one <corrected_text> block.\n"
      "No markdown. No explanation. No code fences.";
  strict.userPrefix = "Return machine-readable XML only for this payload:\n";
  variants.push_back(strict);

  TemplateVariant compact;
  compact.id = "V03_compact_skeleton";
  compact.description = "紧凑骨架模板，减少自然语言发挥空间";
  compact.systemPrompt =
      "Output EXACT XML in this shape only:\n"
      "<validation_json>{\"text_correction\":{\"needed\":false},"
      "\"tool_interventions\":[],\"final_response_action\":\"approve\"}</validation_json>\n"
      "If needed, replace values but keep the same keys.\n"
      "Allowed actions: approve or retry_from_tools.\n"
      "Optional extra tag: <corrected_text>...</corrected_text>\n"
      "Never add any explanation before or after the XML.";
  compact.userPrefix = "Payload JSON:\n";
  variants.push_back(compact);

  TemplateVariant fewshot;
  fewshot.id = "V04_fewshot_guard";
  fewshot.description = "带微型示例的严格模板";
  fewshot.systemPrompt =
      "You are a validation model. Respond in XML only.\n"
      "Good example:\n"
      "<validation_json>{\"text_correction\":{\"needed\":false},"
      "\"tool_interventions\":[],\"final_response_action\":\"approve\"}</validation_json>\n"
      "Bad example: Any summary sentence outside XML.\n"
      "Required keys inside validation_json: text_correction, tool_interventions, "
      "final_response_action. retry_guidance is optional.\n"
      "Allowed final_response_action: approve or retry_from_tools.\n"
      "If text correction is needed, also append <corrected_text>...</corrected_text>.\n"
      "Do not explain your reasoning.";
  fewshot.userPrefix = "Validate this JSON object:\n";
  variants.push_back(fewshot);

  TemplateVariant minimal;
  minimal.id = "V05_minimal_xml_only";
  minimal.description = "极简硬约束模板，优先降低时延和跑偏概率";
  minimal.systemPrompt =
      "Return XML only.\n"
      "First char must be '<'.\n"
      "Output exactly:\n"
      "<validation_json>{\"text_correction\":{\"needed\":false},"
      "\"tool_interventions\":[],\"final_response_action\":\"approve\"}</validation_json>\n"
      "You may change values, and may use retry_from_tools.\n"
      "If corrected text is needed, append <corrected_text>...</corrected_text>.\n"
      "No explanation.";
  minimal.userPrefix = "";
  variants.push_back(minimal);

  TemplateVariant twoPassRepair;
  twoPassRepair.id = "V06_twopass_repair";
  twoPassRepair.description =
      "两阶段 judge + repair，第二轮只负责压成纯 validation_json";
  twoPassRepair.systemPrompt = baseline.systemPrompt;
  twoPassRepair.userPrefix = "";
  twoPassRepair.twoPassRepair = true;
  twoPassRepair.repairSystemPrompt =
      "You are a repair model for a machine parser.\n"
      "Your only task is to convert the previous validator output into pure XML.\n"
      "Return exactly one <validation_json> block.\n"
      "Allowed final_response_action values: approve or retry_from_tools.\n"
      "Required keys: text_correction, tool_interventions, final_response_action.\n"
      "Optional keys: retry_guidance.\n"
      "If corrected text is needed, append exactly one <corrected_text> block.\n"
      "Do not explain. Do not summarize. Output XML only.";
  twoPassRepair.repairUserPrefix =
      "Original payload JSON:\n";
  variants.push_back(twoPassRepair);

  return variants;
}

std::string BuildMarkdownSummary(
    const std::string& validatorModel,
    int iterations,
    const std::vector<TemplateVariant>& variants,
    const std::vector<ValidationCase>& cases,
    const std::vector<RunResult>& runs) {
  std::ostringstream out;
  out << "# Gemma Validator Benchmark\n\n";
  out << "- validator_model: `" << validatorModel << "`\n";
  out << "- iterations_per_case: `" << iterations << "`\n";
  out << "- total_runs: `" << runs.size() << "`\n\n";
  out << "## Variant Summary\n\n";
  out << "| Variant | Runs | Structured | Strict | Expected Action |\n";
  out << "| --- | ---: | ---: | ---: | ---: |\n";

  for (const auto& variant : variants) {
    int structuredOk = 0;
    int strictOk = 0;
    int expectedOk = 0;
    int total = 0;
    for (const auto& run : runs) {
      if (run.variantId != variant.id) continue;
      ++total;
      if (StructuredSuccess(run)) ++structuredOk;
      if (StrictSuccess(run)) ++strictOk;
      for (const auto& testCase : cases) {
        if (testCase.id != run.caseId) continue;
        if (!testCase.expectedAction.empty() &&
            run.parsed.finalResponseAction == testCase.expectedAction &&
            StructuredSuccess(run)) {
          ++expectedOk;
        }
      }
    }
    out << "| `" << variant.id << "` | " << total
        << " | " << structuredOk << "/" << total
        << " | " << strictOk << "/" << total
        << " | " << expectedOk << "/" << total << " |\n";
  }

  out << "\n## Case Details\n\n";
  for (const auto& variant : variants) {
    out << "### " << variant.id << "\n\n";
    out << "- " << variant.description << "\n\n";
    for (const auto& testCase : cases) {
      int structuredOk = 0;
      int strictOk = 0;
      int expectedOk = 0;
      int total = 0;
      for (const auto& run : runs) {
        if (run.variantId != variant.id || run.caseId != testCase.id) continue;
        ++total;
        if (StructuredSuccess(run)) ++structuredOk;
        if (StrictSuccess(run)) ++strictOk;
        if (!testCase.expectedAction.empty() &&
            run.parsed.finalResponseAction == testCase.expectedAction &&
            StructuredSuccess(run)) {
          ++expectedOk;
        }
      }
      if (total == 0) continue;
      out << "- `" << testCase.id << "` structured=" << structuredOk << "/" << total
          << ", strict=" << strictOk << "/" << total
          << ", expected_action=" << expectedOk << "/" << total << "\n";
    }
    out << "\n";
  }
  return out.str();
}

RunResult ExecuteVariantRun(agent::api::HttpLlmClient& client,
                            const TemplateVariant& variant,
                            const ValidationCase& testCase,
                            const std::string& validatorModel,
                            int iteration) {
  RunResult run;
  run.caseId = testCase.id;
  run.variantId = variant.id;
  run.iteration = iteration;

  agent::core::Message judgeMsg;
  judgeMsg.role = agent::core::MessageRole::User;
  judgeMsg.uuid = testCase.id + "-" + std::to_string(iteration) + "-judge";
  judgeMsg.content.push_back(agent::core::ContentBlock::MakeText(
      variant.userPrefix +
      testCase.payload.dump(2, ' ', false, json::error_handler_t::replace)));

  const auto judgeStart = std::chrono::steady_clock::now();
  const std::vector<agent::core::Message> judgeResponse =
      client.SideQuery({judgeMsg}, variant.systemPrompt, validatorModel);
  const auto judgeEnd = std::chrono::steady_clock::now();

  run.judgeElapsedMs = static_cast<long long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          judgeEnd - judgeStart).count());
  run.judgeRawText = CollectText(judgeResponse);
  run.judgeParsed = ParseResponse(run.judgeRawText);

  run.elapsedMs = run.judgeElapsedMs;
  run.rawText = run.judgeRawText;
  run.parsed = run.judgeParsed;

  if (!variant.twoPassRepair) {
    return run;
  }

  run.repairAttempted = !IsTransportErrorText(run.judgeRawText);
  if (!run.repairAttempted) {
    return run;
  }

  json repairPayload;
  repairPayload["original_payload"] = testCase.payload;
  repairPayload["judge_output"] = run.judgeRawText;

  agent::core::Message repairMsg;
  repairMsg.role = agent::core::MessageRole::User;
  repairMsg.uuid = testCase.id + "-" + std::to_string(iteration) + "-repair";
  repairMsg.content.push_back(agent::core::ContentBlock::MakeText(
      variant.repairUserPrefix +
      repairPayload.dump(2, ' ', false, json::error_handler_t::replace)));

  const auto repairStart = std::chrono::steady_clock::now();
  const std::vector<agent::core::Message> repairResponse =
      client.SideQuery({repairMsg}, variant.repairSystemPrompt, validatorModel);
  const auto repairEnd = std::chrono::steady_clock::now();

  run.repairElapsedMs = static_cast<long long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          repairEnd - repairStart).count());
  run.elapsedMs += run.repairElapsedMs;
  run.repairRawText = CollectText(repairResponse);
  run.repairParsed = ParseResponse(run.repairRawText);
  run.rawText = run.repairRawText;
  run.parsed = run.repairParsed;
  return run;
}

int GetEnvInt(const char* name, int fallback) {
  char buffer[32] = {0};
  DWORD len = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
  if (len == 0 || len >= sizeof(buffer)) return fallback;
  return std::max(1, std::atoi(buffer));
}

std::string GetEnvString(const char* name, const std::string& fallback) {
  char buffer[512] = {0};
  DWORD len = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
  if (len == 0 || len >= sizeof(buffer)) return fallback;
  return std::string(buffer, len);
}

bool MatchesFilter(const std::string& value, const std::string& filter) {
  if (filter.empty()) return true;
  std::stringstream stream(filter);
  std::string item;
  while (std::getline(stream, item, ',')) {
    item = Trim(item);
    if (!item.empty() && item == value) return true;
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;

  const std::string projectRoot = DiscoverProjectRoot();
  const std::string buildDir = JoinPath(projectRoot, "build");
  const std::string benchmarkRoot =
      JoinPath(buildDir, "benchmarks\\gemma_validator");
  EnsureDirectoryRecursive(benchmarkRoot);

  const std::string endpoint = GetEnvString(
      "CPP_AGENT_API_ENDPOINT", "http://127.0.0.1:8080/v1/chat/completions");
  const std::string validatorModel = GetEnvString(
      "CPP_AGENT_VALIDATOR_MODEL", "gemma-4-31B-it-Q8_0");
  const int iterations = GetEnvInt("AGENT_GEMMA_BENCH_ITERS", 2);
  const int requestTimeoutMs =
      GetEnvInt("AGENT_GEMMA_BENCH_TIMEOUT_MS", 60000);
  const std::string variantFilter =
      GetEnvString("AGENT_GEMMA_BENCH_VARIANTS", "");
  const std::string caseFilter =
      GetEnvString("AGENT_GEMMA_BENCH_CASES", "");

  agent::core::LlmConfig cfg;
  cfg.apiEndpoint = endpoint;
  cfg.mainModel = validatorModel;
  cfg.validatorModel = validatorModel;
  cfg.fallbackModel = validatorModel;
  cfg.connectTimeoutMs = 30000;
  cfg.requestTimeoutMs = requestTimeoutMs;

  agent::api::HttpLlmClient client(cfg);

  const auto cases = BuildCases();
  const auto variants = BuildVariants();
  std::vector<RunResult> runs;

  std::cout << "Gemma validator benchmark start" << std::endl;
  std::cout << "endpoint=" << endpoint << std::endl;
  std::cout << "validator_model=" << validatorModel << std::endl;
  std::cout << "iterations=" << iterations << std::endl;
  std::cout << "request_timeout_ms=" << requestTimeoutMs << std::endl;
  if (!variantFilter.empty()) std::cout << "variant_filter=" << variantFilter << std::endl;
  if (!caseFilter.empty()) std::cout << "case_filter=" << caseFilter << std::endl;

  for (const auto& variant : variants) {
    if (!MatchesFilter(variant.id, variantFilter)) continue;
    std::cout << "\n[variant] " << variant.id << " - " << variant.description
              << std::endl;
    const std::string variantDir = JoinPath(benchmarkRoot, variant.id);
    EnsureDirectoryRecursive(variantDir);

    for (const auto& testCase : cases) {
      if (!MatchesFilter(testCase.id, caseFilter)) continue;
      std::cout << "  [case] " << testCase.id << " - " << testCase.description
                << std::endl;
      for (int i = 1; i <= iterations; ++i) {
        RunResult run = ExecuteVariantRun(
            client, variant, testCase, validatorModel, i);
        runs.push_back(run);

        json runJson;
        runJson["variant"] = variant.id;
        runJson["case"] = testCase.id;
        runJson["iteration"] = i;
        runJson["elapsed_ms"] = run.elapsedMs;
        runJson["judge_elapsed_ms"] = run.judgeElapsedMs;
        runJson["repair_elapsed_ms"] = run.repairElapsedMs;
        runJson["repair_attempted"] = run.repairAttempted;
        runJson["has_validation_json"] = run.parsed.hasValidationJson;
        runJson["parsed_json"] = run.parsed.parsedJson;
        runJson["no_extra_text"] = run.parsed.noExtraText;
        runJson["final_response_action"] = run.parsed.finalResponseAction;
        runJson["retry_guidance"] = run.parsed.retryGuidance;
        runJson["has_corrected_text"] = run.parsed.hasCorrectedText;
        runJson["raw_text"] = run.rawText;
        runJson["judge_raw_text"] = run.judgeRawText;
        runJson["judge_has_validation_json"] = run.judgeParsed.hasValidationJson;
        runJson["judge_parsed_json"] = run.judgeParsed.parsedJson;
        runJson["judge_no_extra_text"] = run.judgeParsed.noExtraText;
        runJson["judge_final_response_action"] = run.judgeParsed.finalResponseAction;
        runJson["repair_raw_text"] = run.repairRawText;
        runJson["repair_has_validation_json"] = run.repairParsed.hasValidationJson;
        runJson["repair_parsed_json"] = run.repairParsed.parsedJson;
        runJson["repair_no_extra_text"] = run.repairParsed.noExtraText;
        runJson["repair_final_response_action"] = run.repairParsed.finalResponseAction;

        const std::string baseName =
            testCase.id + "_run" + std::to_string(i);
        WriteTextFile(JoinPath(variantDir, baseName + ".json"),
                      runJson.dump(2, ' ', false,
                                   json::error_handler_t::replace));
        WriteTextFile(JoinPath(variantDir, baseName + ".txt"), run.rawText);

        std::cout << "    run=" << i
                  << " structured=" << (StructuredSuccess(run) ? "yes" : "no")
                  << " strict=" << (StrictSuccess(run) ? "yes" : "no")
                  << " action=" << run.parsed.finalResponseAction
                  << " elapsed_ms=" << run.elapsedMs;
        if (variant.twoPassRepair) {
          std::cout << " judge_ms=" << run.judgeElapsedMs
                    << " repair_ms=" << run.repairElapsedMs
                    << " repair_attempted="
                    << (run.repairAttempted ? "yes" : "no");
        }
        std::cout << std::endl;
      }
    }
  }

  const std::string summary =
      BuildMarkdownSummary(validatorModel, iterations, variants, cases, runs);
  WriteTextFile(JoinPath(benchmarkRoot, "summary.md"), summary);

  json summaryJson = json::array();
  for (const auto& run : runs) {
    summaryJson.push_back({
        {"variant", run.variantId},
        {"case", run.caseId},
        {"iteration", run.iteration},
        {"elapsed_ms", run.elapsedMs},
        {"judge_elapsed_ms", run.judgeElapsedMs},
        {"repair_elapsed_ms", run.repairElapsedMs},
        {"repair_attempted", run.repairAttempted},
        {"has_validation_json", run.parsed.hasValidationJson},
        {"parsed_json", run.parsed.parsedJson},
        {"no_extra_text", run.parsed.noExtraText},
        {"final_response_action", run.parsed.finalResponseAction},
        {"retry_guidance", run.parsed.retryGuidance},
        {"has_corrected_text", run.parsed.hasCorrectedText},
        {"judge_has_validation_json", run.judgeParsed.hasValidationJson},
        {"judge_parsed_json", run.judgeParsed.parsedJson},
        {"judge_no_extra_text", run.judgeParsed.noExtraText},
        {"judge_final_response_action", run.judgeParsed.finalResponseAction},
        {"repair_has_validation_json", run.repairParsed.hasValidationJson},
        {"repair_parsed_json", run.repairParsed.parsedJson},
        {"repair_no_extra_text", run.repairParsed.noExtraText},
        {"repair_final_response_action", run.repairParsed.finalResponseAction},
    });
  }
  WriteTextFile(JoinPath(benchmarkRoot, "summary.json"),
                summaryJson.dump(2, ' ', false,
                                 json::error_handler_t::replace));

  std::cout << "\nBenchmark artifacts: " << benchmarkRoot << std::endl;
  std::cout << "Summary written to summary.md" << std::endl;
  return 0;
}
