#include "infra/SessionManager.h"
#include "infra/ProtoLite.h"
#include "third_party/nlohmann_json.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>
#include <windows.h>

namespace agent {
namespace infra {

namespace {

static const char kLegacySnapshotMagic[] = "AGSNAP2";
static const std::uint16_t kLegacySnapshotVersion = 2;
static const std::uint16_t kLegacySnapshotReserved = 0;
static const std::uint16_t kSnapshotVersion = 3;

enum SnapshotFieldTag {
  kFieldSessionId = 1,
  kFieldTimestamp = 2,
  kFieldMetadata = 3,
  kFieldMessage = 4,
  kFieldSubTask = 5,
  kFieldExecutor = 6
};

enum ProtoSnapshotFieldTag {
  kProtoFieldFormatVersion = 1,
  kProtoFieldSessionId = 2,
  kProtoFieldTimestamp = 3,
  kProtoFieldMetadata = 4,
  kProtoFieldMessage = 5,
  kProtoFieldSubTask = 6,
  kProtoFieldExecutor = 7
};

enum MetadataFieldTag {
  kMetadataId = 1,
  kMetadataTurnCount = 2
};

enum MessageFieldTag {
  kMessageRole = 1,
  kMessageUuid = 2,
  kMessageIsMeta = 3,
  kMessageStopReason = 4,
  kMessageIsApiError = 5,
  kMessageUsage = 6,
  kMessageBlock = 7
};

enum UsageFieldTag {
  kUsageInput = 1,
  kUsageOutput = 2,
  kUsageCacheRead = 3,
  kUsageCacheCreate = 4
};

enum BlockFieldTag {
  kBlockType = 1,
  kBlockText = 2,
  kBlockToolUseId = 3,
  kBlockToolName = 4,
  kBlockToolInput = 5,
  kBlockResultToolUseId = 6,
  kBlockResultContent = 7,
  kBlockResultIsError = 8
};

enum TaskFieldTag {
  kTaskLifecycleId = 1,
  kTaskLifecycleDirective = 2,
  kTaskLifecycleWorktreeNotice = 3,
  kTaskLifecyclePlaceholder = 4,
  kTaskLifecycleSummary = 5,
  kTaskLifecycleState = 6,
  kTaskLifecycleCreatedAt = 7,
  kTaskLifecycleUpdatedAt = 8,
  kTaskLifecycleTask = 9,
  kTaskLifecycleAssignedExecutor = 10,
  kTaskLifecycleFailureReason = 11,
  kTaskLifecycleAttemptCount = 12,
  kTaskLifecycleCheckpoint = 13
};

enum TaskDefinitionFieldTag {
  kTaskPrompt = 1,
  kTaskDescription = 2,
  kTaskSubagentType = 3,
  kTaskModel = 4,
  kTaskPriority = 5,
  kTaskRequiredSlots = 6,
  kTaskRunInBackground = 7,
  kTaskIsolation = 8,
  kTaskCwd = 9,
  kTaskParentCwd = 10,
  kTaskWorktreeCwd = 11,
  kTaskName = 12,
  kTaskTeamName = 13,
  kTaskMode = 14
};

enum CheckpointFieldTag {
  kCheckpointId = 1,
  kCheckpointCursor = 2,
  kCheckpointSavedAt = 3,
  kCheckpointResumable = 4
};

enum ExecutorFieldTag {
  kExecutorId = 1,
  kExecutorHost = 2,
  kExecutorMaxParallel = 3,
  kExecutorRunning = 4,
  kExecutorWeight = 5,
  kExecutorHealthy = 6,
  kExecutorCheckpointResume = 7,
  kExecutorState = 8,
  kExecutorLastHeartbeat = 9,
  kExecutorLastError = 10
};

std::string TimestampNow() {
  const std::time_t t = std::time(nullptr);
  std::tm tm;
  localtime_s(&tm, &t);
  std::ostringstream stream;
  stream << std::put_time(&tm, "%Y%m%dT%H%M%S");
  return stream.str();
}

bool EnsureDirectoryRecursive(const std::string& path) {
  if (path.empty()) return false;
  std::string normalized = path;
  std::replace(normalized.begin(), normalized.end(), '/', '\\');
  std::size_t cursor = 0;
  if (normalized.size() >= 2 && normalized[1] == ':') cursor = 3;
  while (cursor <= normalized.size()) {
    const std::size_t next = normalized.find('\\', cursor);
    const std::string current =
        next == std::string::npos ? normalized : normalized.substr(0, next);
    if (!current.empty()) {
      const DWORD attrs = GetFileAttributesA(current.c_str());
      if (attrs == INVALID_FILE_ATTRIBUTES) {
        if (!CreateDirectoryA(current.c_str(), nullptr)) {
          if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
        }
      }
    }
    if (next == std::string::npos) break;
    cursor = next + 1;
  }
  return true;
}

void WriteTextFile(const std::string& path, const std::string& content) {
  EnsureDirectoryRecursive(path.substr(0, path.find_last_of("\\/")));
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (output) output << content;
}

bool WriteBinaryFile(const std::string& path, const std::string& content) {
  EnsureDirectoryRecursive(path.substr(0, path.find_last_of("\\/")));
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    return false;
  }
  output.write(content.data(), static_cast<std::streamsize>(content.size()));
  return output.good();
}

std::string ReadTextFile(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return std::string();
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

std::string EscapeField(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (char ch : value) {
    switch (ch) {
      case '\\':
        escaped += "\\\\";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        escaped.push_back(ch);
        break;
    }
  }
  return escaped;
}

std::string UnescapeField(const std::string& value) {
  std::string unescaped;
  unescaped.reserve(value.size());
  bool escape = false;
  for (char ch : value) {
    if (!escape) {
      if (ch == '\\') {
        escape = true;
      } else {
        unescaped.push_back(ch);
      }
      continue;
    }

    switch (ch) {
      case 'n':
        unescaped.push_back('\n');
        break;
      case 'r':
        unescaped.push_back('\r');
        break;
      case 't':
        unescaped.push_back('\t');
        break;
      case '\\':
        unescaped.push_back('\\');
        break;
      default:
        unescaped.push_back(ch);
        break;
    }
    escape = false;
  }
  if (escape) {
    unescaped.push_back('\\');
  }
  return unescaped;
}

std::string BoolToString(bool value) {
  return value ? "1" : "0";
}

bool ParseBool(const std::string& value) {
  return value == "1" || value == "true" || value == "True";
}

const char* RoleToString(agent::core::MessageRole role) {
  switch (role) {
    case agent::core::MessageRole::User:
      return "user";
    case agent::core::MessageRole::Assistant:
      return "assistant";
    case agent::core::MessageRole::System:
      return "system";
  }
  return "user";
}

agent::core::MessageRole RoleFromString(const std::string& value) {
  if (value == "assistant") {
    return agent::core::MessageRole::Assistant;
  }
  if (value == "system") {
    return agent::core::MessageRole::System;
  }
  return agent::core::MessageRole::User;
}

const char* BlockTypeToString(agent::core::BlockType type) {
  switch (type) {
    case agent::core::BlockType::Text:
      return "text";
    case agent::core::BlockType::ToolUse:
      return "tool_use";
    case agent::core::BlockType::ToolResult:
      return "tool_result";
  }
  return "text";
}

agent::core::BlockType BlockTypeFromString(const std::string& value) {
  if (value == "tool_use") {
    return agent::core::BlockType::ToolUse;
  }
  if (value == "tool_result") {
    return agent::core::BlockType::ToolResult;
  }
  return agent::core::BlockType::Text;
}

const char* TaskStateToString(agent::agents::SubAgentTaskState state) {
  switch (state) {
    case agent::agents::SubAgentTaskState::Pending:
      return "pending";
    case agent::agents::SubAgentTaskState::Running:
      return "running";
    case agent::agents::SubAgentTaskState::Completed:
      return "completed";
    case agent::agents::SubAgentTaskState::Failed:
      return "failed";
    case agent::agents::SubAgentTaskState::Cancelled:
      return "cancelled";
  }
  return "pending";
}

agent::agents::SubAgentTaskState TaskStateFromString(const std::string& value) {
  if (value == "running") {
    return agent::agents::SubAgentTaskState::Running;
  }
  if (value == "completed") {
    return agent::agents::SubAgentTaskState::Completed;
  }
  if (value == "failed") {
    return agent::agents::SubAgentTaskState::Failed;
  }
  if (value == "cancelled") {
    return agent::agents::SubAgentTaskState::Cancelled;
  }
  return agent::agents::SubAgentTaskState::Pending;
}

int ExecutorStateToInt(agent::agents::SubAgentExecutorState state) {
  return static_cast<int>(state);
}

agent::agents::SubAgentExecutorState ExecutorStateFromInt(int value) {
  switch (value) {
    case 1:
      return agent::agents::SubAgentExecutorState::Busy;
    case 2:
      return agent::agents::SubAgentExecutorState::Recovering;
    case 3:
      return agent::agents::SubAgentExecutorState::Offline;
    default:
      return agent::agents::SubAgentExecutorState::Idle;
  }
}

std::string ReadSnapshotValue(
    const std::vector<std::string>& lines,
    const std::string& key,
    const std::string& fallback = std::string()) {
  const std::string prefix = key + "=";
  for (const auto& line : lines) {
    if (line.find(prefix) == 0) {
      return UnescapeField(line.substr(prefix.size()));
    }
  }
  return fallback;
}

int ReadSnapshotInt(
    const std::vector<std::string>& lines,
    const std::string& key,
    int fallback = 0) {
  const std::string value = ReadSnapshotValue(lines, key);
  return value.empty() ? fallback : std::atoi(value.c_str());
}

long long ReadSnapshotLongLong(
    const std::vector<std::string>& lines,
    const std::string& key,
    long long fallback = 0) {
  const std::string value = ReadSnapshotValue(lines, key);
  return value.empty() ? fallback : _strtoi64(value.c_str(), nullptr, 10);
}

void AppendU16(std::string* output, std::uint16_t value) {
  output->push_back(static_cast<char>(value & 0xFF));
  output->push_back(static_cast<char>((value >> 8) & 0xFF));
}

void AppendU32(std::string* output, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    output->push_back(static_cast<char>((value >> (i * 8)) & 0xFF));
  }
}

void AppendI32(std::string* output, int value) {
  AppendU32(output, static_cast<std::uint32_t>(value));
}

void AppendI64(std::string* output, long long value) {
  for (int i = 0; i < 8; ++i) {
    output->push_back(static_cast<char>(
        (static_cast<std::uint64_t>(value) >> (i * 8)) & 0xFF));
  }
}

bool ReadU16(const std::string& input, std::size_t* cursor, std::uint16_t* value) {
  if (*cursor + 2 > input.size()) {
    return false;
  }
  *value = static_cast<std::uint16_t>(
      static_cast<unsigned char>(input[*cursor])) |
           (static_cast<std::uint16_t>(
                static_cast<unsigned char>(input[*cursor + 1]))
            << 8);
  *cursor += 2;
  return true;
}

bool ReadU32(const std::string& input, std::size_t* cursor, std::uint32_t* value) {
  if (*cursor + 4 > input.size()) {
    return false;
  }
  *value = 0;
  for (int i = 0; i < 4; ++i) {
    *value |= static_cast<std::uint32_t>(
                  static_cast<unsigned char>(input[*cursor + i]))
              << (i * 8);
  }
  *cursor += 4;
  return true;
}

bool ReadI32(const std::string& input, std::size_t* cursor, int* value) {
  std::uint32_t temp = 0;
  if (!ReadU32(input, cursor, &temp)) {
    return false;
  }
  *value = static_cast<int>(temp);
  return true;
}

bool ReadI64(const std::string& input, std::size_t* cursor, long long* value) {
  if (*cursor + 8 > input.size()) {
    return false;
  }
  std::uint64_t temp = 0;
  for (int i = 0; i < 8; ++i) {
    temp |= static_cast<std::uint64_t>(
                static_cast<unsigned char>(input[*cursor + i]))
            << (i * 8);
  }
  *cursor += 8;
  *value = static_cast<long long>(temp);
  return true;
}

void AppendField(std::string* output, std::uint16_t tag, const std::string& payload) {
  AppendU16(output, tag);
  AppendU32(output, static_cast<std::uint32_t>(payload.size()));
  output->append(payload);
}

std::string MakeStringFieldPayload(const std::string& value) {
  return value;
}

std::string MakeBoolFieldPayload(bool value) {
  std::string payload;
  payload.push_back(value ? '\1' : '\0');
  return payload;
}

std::string MakeI32FieldPayload(int value) {
  std::string payload;
  AppendI32(&payload, value);
  return payload;
}

std::string MakeI64FieldPayload(long long value) {
  std::string payload;
  AppendI64(&payload, value);
  return payload;
}

bool ReadBoolPayload(const std::string& payload, bool* value) {
  if (payload.empty() || value == nullptr) {
    return false;
  }
  *value = payload[0] != '\0';
  return true;
}

bool ReadI32Payload(const std::string& payload, int* value) {
  std::size_t cursor = 0;
  return value != nullptr && ReadI32(payload, &cursor, value);
}

bool ReadI64Payload(const std::string& payload, long long* value) {
  std::size_t cursor = 0;
  return value != nullptr && ReadI64(payload, &cursor, value);
}

bool ReadNextField(
    const std::string& input,
    std::size_t* cursor,
    std::uint16_t* tag,
    std::string* payload) {
  std::uint16_t fieldTag = 0;
  std::uint32_t length = 0;
  if (!ReadU16(input, cursor, &fieldTag) || !ReadU32(input, cursor, &length)) {
    return false;
  }
  if (*cursor + length > input.size()) {
    return false;
  }
  if (tag != nullptr) {
    *tag = fieldTag;
  }
  if (payload != nullptr) {
    *payload = input.substr(*cursor, length);
  }
  *cursor += length;
  return true;
}

std::string SerializeUsage(const agent::core::Usage& usage) {
  std::string output;
  AppendField(&output, kUsageInput, MakeI32FieldPayload(usage.inputTokens));
  AppendField(&output, kUsageOutput, MakeI32FieldPayload(usage.outputTokens));
  AppendField(&output, kUsageCacheRead,
              MakeI32FieldPayload(usage.cacheReadInputTokens));
  AppendField(&output, kUsageCacheCreate,
              MakeI32FieldPayload(usage.cacheCreationInputTokens));
  return output;
}

bool DeserializeUsage(const std::string& payload, agent::core::Usage* usage) {
  if (usage == nullptr) {
    return false;
  }
  std::size_t cursor = 0;
  while (cursor < payload.size()) {
    std::uint16_t tag = 0;
    std::string fieldPayload;
    if (!ReadNextField(payload, &cursor, &tag, &fieldPayload)) {
      return false;
    }
    switch (tag) {
      case kUsageInput:
        ReadI32Payload(fieldPayload, &usage->inputTokens);
        break;
      case kUsageOutput:
        ReadI32Payload(fieldPayload, &usage->outputTokens);
        break;
      case kUsageCacheRead:
        ReadI32Payload(fieldPayload, &usage->cacheReadInputTokens);
        break;
      case kUsageCacheCreate:
        ReadI32Payload(fieldPayload, &usage->cacheCreationInputTokens);
        break;
      default:
        break;
    }
  }
  return true;
}

std::string SerializeBlock(const agent::core::ContentBlock& block) {
  std::string output;
  AppendField(&output, kBlockType, MakeI32FieldPayload(static_cast<int>(block.type)));
  if (block.type == agent::core::BlockType::Text) {
    AppendField(&output, kBlockText, MakeStringFieldPayload(block.asText.text));
  } else if (block.type == agent::core::BlockType::ToolUse) {
    AppendField(&output, kBlockToolUseId,
                MakeStringFieldPayload(block.asToolUse.id));
    AppendField(&output, kBlockToolName,
                MakeStringFieldPayload(block.asToolUse.name));
    AppendField(&output, kBlockToolInput,
                MakeStringFieldPayload(block.asToolUse.inputJson));
  } else if (block.type == agent::core::BlockType::ToolResult) {
    AppendField(&output, kBlockResultToolUseId,
                MakeStringFieldPayload(block.asToolResult.toolUseId));
    AppendField(&output, kBlockResultContent,
                MakeStringFieldPayload(block.asToolResult.content));
    AppendField(&output, kBlockResultIsError,
                MakeBoolFieldPayload(block.asToolResult.isError));
  }
  return output;
}

bool DeserializeBlock(
    const std::string& payload,
    agent::core::ContentBlock* block) {
  if (block == nullptr) {
    return false;
  }
  std::size_t cursor = 0;
  int typeValue = 0;
  std::string text;
  std::string toolUseId;
  std::string toolName;
  std::string toolInput;
  std::string resultToolUseId;
  std::string resultContent;
  bool resultIsError = false;
  while (cursor < payload.size()) {
    std::uint16_t tag = 0;
    std::string fieldPayload;
    if (!ReadNextField(payload, &cursor, &tag, &fieldPayload)) {
      return false;
    }
    switch (tag) {
      case kBlockType:
        ReadI32Payload(fieldPayload, &typeValue);
        break;
      case kBlockText:
        text = fieldPayload;
        break;
      case kBlockToolUseId:
        toolUseId = fieldPayload;
        break;
      case kBlockToolName:
        toolName = fieldPayload;
        break;
      case kBlockToolInput:
        toolInput = fieldPayload;
        break;
      case kBlockResultToolUseId:
        resultToolUseId = fieldPayload;
        break;
      case kBlockResultContent:
        resultContent = fieldPayload;
        break;
      case kBlockResultIsError:
        ReadBoolPayload(fieldPayload, &resultIsError);
        break;
      default:
        break;
    }
  }

  const agent::core::BlockType type =
      static_cast<agent::core::BlockType>(typeValue);
  if (type == agent::core::BlockType::ToolUse) {
    *block = agent::core::ContentBlock::MakeToolUse(toolUseId, toolName, toolInput);
  } else if (type == agent::core::BlockType::ToolResult) {
    *block = agent::core::ContentBlock::MakeToolResult(
        resultToolUseId, resultContent, resultIsError);
  } else {
    *block = agent::core::ContentBlock::MakeText(text);
  }
  return true;
}

std::string SerializeMessage(const agent::core::Message& message) {
  std::string output;
  AppendField(&output, kMessageRole,
              MakeI32FieldPayload(static_cast<int>(message.role)));
  AppendField(&output, kMessageUuid, MakeStringFieldPayload(message.uuid));
  AppendField(&output, kMessageIsMeta, MakeBoolFieldPayload(message.isMeta));
  AppendField(&output, kMessageStopReason,
              MakeStringFieldPayload(message.stopReason));
  AppendField(&output, kMessageIsApiError,
              MakeBoolFieldPayload(message.isApiErrorMessage));
  AppendField(&output, kMessageUsage, SerializeUsage(message.usage));
  for (const auto& block : message.content) {
    AppendField(&output, kMessageBlock, SerializeBlock(block));
  }
  return output;
}

bool DeserializeMessage(
    const std::string& payload,
    agent::core::Message* message) {
  if (message == nullptr) {
    return false;
  }
  std::size_t cursor = 0;
  while (cursor < payload.size()) {
    std::uint16_t tag = 0;
    std::string fieldPayload;
    if (!ReadNextField(payload, &cursor, &tag, &fieldPayload)) {
      return false;
    }
    switch (tag) {
      case kMessageRole: {
        int roleValue = 0;
        ReadI32Payload(fieldPayload, &roleValue);
        message->role = static_cast<agent::core::MessageRole>(roleValue);
        break;
      }
      case kMessageUuid:
        message->uuid = fieldPayload;
        break;
      case kMessageIsMeta:
        ReadBoolPayload(fieldPayload, &message->isMeta);
        break;
      case kMessageStopReason:
        message->stopReason = fieldPayload;
        break;
      case kMessageIsApiError:
        ReadBoolPayload(fieldPayload, &message->isApiErrorMessage);
        break;
      case kMessageUsage:
        DeserializeUsage(fieldPayload, &message->usage);
        break;
      case kMessageBlock: {
        agent::core::ContentBlock block;
        if (!DeserializeBlock(fieldPayload, &block)) {
          return false;
        }
        message->content.push_back(block);
        break;
      }
      default:
        break;
    }
  }
  return true;
}

std::string SerializeTaskDefinition(const agent::agents::SubAgentTask& task) {
  std::string output;
  AppendField(&output, kTaskPrompt, MakeStringFieldPayload(task.prompt));
  AppendField(&output, kTaskDescription,
              MakeStringFieldPayload(task.description));
  AppendField(&output, kTaskSubagentType,
              MakeStringFieldPayload(task.subagentType));
  AppendField(&output, kTaskModel, MakeStringFieldPayload(task.model));
  AppendField(&output, kTaskPriority, MakeI32FieldPayload(task.priority));
  AppendField(&output, kTaskRequiredSlots,
              MakeI32FieldPayload(task.requiredExecutorSlots));
  AppendField(&output, kTaskRunInBackground,
              MakeBoolFieldPayload(task.runInBackground));
  AppendField(&output, kTaskIsolation, MakeStringFieldPayload(task.isolation));
  AppendField(&output, kTaskCwd, MakeStringFieldPayload(task.cwd));
  AppendField(&output, kTaskParentCwd, MakeStringFieldPayload(task.parentCwd));
  AppendField(&output, kTaskWorktreeCwd,
              MakeStringFieldPayload(task.worktreeCwd));
  AppendField(&output, kTaskName, MakeStringFieldPayload(task.name));
  AppendField(&output, kTaskTeamName, MakeStringFieldPayload(task.teamName));
  AppendField(&output, kTaskMode, MakeStringFieldPayload(task.mode));
  return output;
}

bool DeserializeTaskDefinition(
    const std::string& payload,
    agent::agents::SubAgentTask* task) {
  if (task == nullptr) {
    return false;
  }
  std::size_t cursor = 0;
  while (cursor < payload.size()) {
    std::uint16_t tag = 0;
    std::string fieldPayload;
    if (!ReadNextField(payload, &cursor, &tag, &fieldPayload)) {
      return false;
    }
    switch (tag) {
      case kTaskPrompt:
        task->prompt = fieldPayload;
        break;
      case kTaskDescription:
        task->description = fieldPayload;
        break;
      case kTaskSubagentType:
        task->subagentType = fieldPayload;
        break;
      case kTaskModel:
        task->model = fieldPayload;
        break;
      case kTaskPriority:
        ReadI32Payload(fieldPayload, &task->priority);
        break;
      case kTaskRequiredSlots:
        ReadI32Payload(fieldPayload, &task->requiredExecutorSlots);
        break;
      case kTaskRunInBackground:
        ReadBoolPayload(fieldPayload, &task->runInBackground);
        break;
      case kTaskIsolation:
        task->isolation = fieldPayload;
        break;
      case kTaskCwd:
        task->cwd = fieldPayload;
        break;
      case kTaskParentCwd:
        task->parentCwd = fieldPayload;
        break;
      case kTaskWorktreeCwd:
        task->worktreeCwd = fieldPayload;
        break;
      case kTaskName:
        task->name = fieldPayload;
        break;
      case kTaskTeamName:
        task->teamName = fieldPayload;
        break;
      case kTaskMode:
        task->mode = fieldPayload;
        break;
      default:
        break;
    }
  }
  return true;
}

std::string SerializeCheckpoint(
    const agent::agents::SubAgentTaskCheckpoint& checkpoint) {
  std::string output;
  AppendField(&output, kCheckpointId,
              MakeStringFieldPayload(checkpoint.checkpointId));
  AppendField(&output, kCheckpointCursor,
              MakeStringFieldPayload(checkpoint.resumeCursor));
  AppendField(&output, kCheckpointSavedAt,
              MakeI64FieldPayload(checkpoint.savedAtUnixMs));
  AppendField(&output, kCheckpointResumable,
              MakeBoolFieldPayload(checkpoint.resumable));
  return output;
}

bool DeserializeCheckpoint(
    const std::string& payload,
    agent::agents::SubAgentTaskCheckpoint* checkpoint) {
  if (checkpoint == nullptr) {
    return false;
  }
  std::size_t cursor = 0;
  while (cursor < payload.size()) {
    std::uint16_t tag = 0;
    std::string fieldPayload;
    if (!ReadNextField(payload, &cursor, &tag, &fieldPayload)) {
      return false;
    }
    switch (tag) {
      case kCheckpointId:
        checkpoint->checkpointId = fieldPayload;
        break;
      case kCheckpointCursor:
        checkpoint->resumeCursor = fieldPayload;
        break;
      case kCheckpointSavedAt:
        ReadI64Payload(fieldPayload, &checkpoint->savedAtUnixMs);
        break;
      case kCheckpointResumable:
        ReadBoolPayload(fieldPayload, &checkpoint->resumable);
        break;
      default:
        break;
    }
  }
  return true;
}

std::string SerializeTaskLifecycle(
    const agent::agents::SubAgentTaskLifecycle& task) {
  std::string output;
  AppendField(&output, kTaskLifecycleId, MakeStringFieldPayload(task.taskId));
  AppendField(&output, kTaskLifecycleDirective,
              MakeStringFieldPayload(task.directive));
  AppendField(&output, kTaskLifecycleWorktreeNotice,
              MakeStringFieldPayload(task.worktreeNotice));
  AppendField(&output, kTaskLifecyclePlaceholder,
              MakeStringFieldPayload(task.placeholderResult));
  AppendField(&output, kTaskLifecycleSummary,
              MakeStringFieldPayload(task.summary));
  AppendField(&output, kTaskLifecycleState,
              MakeI32FieldPayload(static_cast<int>(task.state)));
  AppendField(&output, kTaskLifecycleCreatedAt,
              MakeI64FieldPayload(task.createdAtUnixMs));
  AppendField(&output, kTaskLifecycleUpdatedAt,
              MakeI64FieldPayload(task.updatedAtUnixMs));
  AppendField(&output, kTaskLifecycleTask, SerializeTaskDefinition(task.task));
  AppendField(&output, kTaskLifecycleAssignedExecutor,
              MakeStringFieldPayload(task.assignedExecutorId));
  AppendField(&output, kTaskLifecycleFailureReason,
              MakeStringFieldPayload(task.lastFailureReason));
  AppendField(&output, kTaskLifecycleAttemptCount,
              MakeI32FieldPayload(task.attemptCount));
  AppendField(&output, kTaskLifecycleCheckpoint,
              SerializeCheckpoint(task.checkpoint));
  return output;
}

bool DeserializeTaskLifecycle(
    const std::string& payload,
    agent::agents::SubAgentTaskLifecycle* task) {
  if (task == nullptr) {
    return false;
  }
  std::size_t cursor = 0;
  while (cursor < payload.size()) {
    std::uint16_t tag = 0;
    std::string fieldPayload;
    if (!ReadNextField(payload, &cursor, &tag, &fieldPayload)) {
      return false;
    }
    switch (tag) {
      case kTaskLifecycleId:
        task->taskId = fieldPayload;
        break;
      case kTaskLifecycleDirective:
        task->directive = fieldPayload;
        break;
      case kTaskLifecycleWorktreeNotice:
        task->worktreeNotice = fieldPayload;
        break;
      case kTaskLifecyclePlaceholder:
        task->placeholderResult = fieldPayload;
        break;
      case kTaskLifecycleSummary:
        task->summary = fieldPayload;
        break;
      case kTaskLifecycleState: {
        int stateValue = 0;
        ReadI32Payload(fieldPayload, &stateValue);
        task->state = static_cast<agent::agents::SubAgentTaskState>(stateValue);
        break;
      }
      case kTaskLifecycleCreatedAt:
        ReadI64Payload(fieldPayload, &task->createdAtUnixMs);
        break;
      case kTaskLifecycleUpdatedAt:
        ReadI64Payload(fieldPayload, &task->updatedAtUnixMs);
        break;
      case kTaskLifecycleTask:
        DeserializeTaskDefinition(fieldPayload, &task->task);
        break;
      case kTaskLifecycleAssignedExecutor:
        task->assignedExecutorId = fieldPayload;
        break;
      case kTaskLifecycleFailureReason:
        task->lastFailureReason = fieldPayload;
        break;
      case kTaskLifecycleAttemptCount:
        ReadI32Payload(fieldPayload, &task->attemptCount);
        break;
      case kTaskLifecycleCheckpoint:
        DeserializeCheckpoint(fieldPayload, &task->checkpoint);
        break;
      default:
        break;
    }
  }
  return true;
}

std::string SerializeExecutor(
    const agent::agents::SubAgentExecutorSlot& executor) {
  std::string output;
  AppendField(&output, kExecutorId,
              MakeStringFieldPayload(executor.executorId));
  AppendField(&output, kExecutorHost,
              MakeStringFieldPayload(executor.hostName));
  AppendField(&output, kExecutorMaxParallel,
              MakeI32FieldPayload(executor.maxParallelTasks));
  AppendField(&output, kExecutorRunning,
              MakeI32FieldPayload(executor.runningTasks));
  AppendField(&output, kExecutorWeight,
              MakeI32FieldPayload(executor.weight));
  AppendField(&output, kExecutorHealthy,
              MakeBoolFieldPayload(executor.healthy));
  AppendField(&output, kExecutorCheckpointResume,
              MakeBoolFieldPayload(executor.supportsCheckpointResume));
  AppendField(&output, kExecutorState,
              MakeI32FieldPayload(ExecutorStateToInt(executor.state)));
  AppendField(&output, kExecutorLastHeartbeat,
              MakeI64FieldPayload(executor.lastHeartbeatUnixMs));
  AppendField(&output, kExecutorLastError,
              MakeStringFieldPayload(executor.lastError));
  return output;
}

bool DeserializeExecutor(
    const std::string& payload,
    agent::agents::SubAgentExecutorSlot* executor) {
  if (executor == nullptr) {
    return false;
  }
  std::size_t cursor = 0;
  while (cursor < payload.size()) {
    std::uint16_t tag = 0;
    std::string fieldPayload;
    if (!ReadNextField(payload, &cursor, &tag, &fieldPayload)) {
      return false;
    }
    switch (tag) {
      case kExecutorId:
        executor->executorId = fieldPayload;
        break;
      case kExecutorHost:
        executor->hostName = fieldPayload;
        break;
      case kExecutorMaxParallel:
        ReadI32Payload(fieldPayload, &executor->maxParallelTasks);
        break;
      case kExecutorRunning:
        ReadI32Payload(fieldPayload, &executor->runningTasks);
        break;
      case kExecutorWeight:
        ReadI32Payload(fieldPayload, &executor->weight);
        break;
      case kExecutorHealthy:
        ReadBoolPayload(fieldPayload, &executor->healthy);
        break;
      case kExecutorCheckpointResume:
        ReadBoolPayload(fieldPayload, &executor->supportsCheckpointResume);
        break;
      case kExecutorState: {
        int state = 0;
        ReadI32Payload(fieldPayload, &state);
        executor->state = ExecutorStateFromInt(state);
        break;
      }
      case kExecutorLastHeartbeat:
        ReadI64Payload(fieldPayload, &executor->lastHeartbeatUnixMs);
        break;
      case kExecutorLastError:
        executor->lastError = fieldPayload;
        break;
      default:
        break;
    }
  }
  return true;
}

std::string SerializeMetadata(const agent::core::SessionMetadata& metadata) {
  std::string output;
  AppendField(&output, kMetadataId, MakeStringFieldPayload(metadata.id));
  AppendField(&output, kMetadataTurnCount,
              MakeI32FieldPayload(metadata.turnCount));
  return output;
}

bool DeserializeMetadata(
    const std::string& payload,
    agent::core::SessionMetadata* metadata) {
  if (metadata == nullptr) {
    return false;
  }
  std::size_t cursor = 0;
  while (cursor < payload.size()) {
    std::uint16_t tag = 0;
    std::string fieldPayload;
    if (!ReadNextField(payload, &cursor, &tag, &fieldPayload)) {
      return false;
    }
    switch (tag) {
      case kMetadataId:
        metadata->id = fieldPayload;
        break;
      case kMetadataTurnCount:
        ReadI32Payload(fieldPayload, &metadata->turnCount);
        break;
      default:
        break;
    }
  }
  return true;
}

std::string SerializeProtoUsage(const agent::core::Usage& usage) {
  std::string output;
  protolite::WriteInt32(&output, kUsageInput, usage.inputTokens);
  protolite::WriteInt32(&output, kUsageOutput, usage.outputTokens);
  protolite::WriteInt32(
      &output, kUsageCacheRead, usage.cacheReadInputTokens);
  protolite::WriteInt32(
      &output, kUsageCacheCreate, usage.cacheCreationInputTokens);
  return output;
}

bool DeserializeProtoUsage(
    const std::string& payload,
    agent::core::Usage* usage) {
  if (usage == nullptr) {
    return false;
  }
  protolite::Reader reader(payload);
  protolite::Field field;
  while (reader.ReadField(&field)) {
    switch (field.number) {
      case kUsageInput:
        protolite::FieldToInt32(field, &usage->inputTokens);
        break;
      case kUsageOutput:
        protolite::FieldToInt32(field, &usage->outputTokens);
        break;
      case kUsageCacheRead:
        protolite::FieldToInt32(field, &usage->cacheReadInputTokens);
        break;
      case kUsageCacheCreate:
        protolite::FieldToInt32(field, &usage->cacheCreationInputTokens);
        break;
      default:
        break;
    }
  }
  return reader.ok();
}

std::string SerializeProtoBlock(const agent::core::ContentBlock& block) {
  std::string output;
  protolite::WriteInt32(&output, kBlockType, static_cast<int>(block.type));
  if (block.type == agent::core::BlockType::Text) {
    protolite::WriteString(&output, kBlockText, block.asText.text);
  } else if (block.type == agent::core::BlockType::ToolUse) {
    protolite::WriteString(&output, kBlockToolUseId, block.asToolUse.id);
    protolite::WriteString(&output, kBlockToolName, block.asToolUse.name);
    protolite::WriteString(&output, kBlockToolInput, block.asToolUse.inputJson);
  } else if (block.type == agent::core::BlockType::ToolResult) {
    protolite::WriteString(
        &output, kBlockResultToolUseId, block.asToolResult.toolUseId);
    protolite::WriteString(
        &output, kBlockResultContent, block.asToolResult.content);
    protolite::WriteBool(
        &output, kBlockResultIsError, block.asToolResult.isError);
  }
  return output;
}

bool DeserializeProtoBlock(
    const std::string& payload,
    agent::core::ContentBlock* block) {
  if (block == nullptr) {
    return false;
  }
  int typeValue = 0;
  std::string text;
  std::string toolUseId;
  std::string toolName;
  std::string toolInput;
  std::string resultToolUseId;
  std::string resultContent;
  bool resultIsError = false;
  protolite::Reader reader(payload);
  protolite::Field field;
  while (reader.ReadField(&field)) {
    switch (field.number) {
      case kBlockType:
        protolite::FieldToInt32(field, &typeValue);
        break;
      case kBlockText:
        protolite::FieldToString(field, &text);
        break;
      case kBlockToolUseId:
        protolite::FieldToString(field, &toolUseId);
        break;
      case kBlockToolName:
        protolite::FieldToString(field, &toolName);
        break;
      case kBlockToolInput:
        protolite::FieldToString(field, &toolInput);
        break;
      case kBlockResultToolUseId:
        protolite::FieldToString(field, &resultToolUseId);
        break;
      case kBlockResultContent:
        protolite::FieldToString(field, &resultContent);
        break;
      case kBlockResultIsError:
        protolite::FieldToBool(field, &resultIsError);
        break;
      default:
        break;
    }
  }
  if (!reader.ok()) {
    return false;
  }
  const agent::core::BlockType type =
      static_cast<agent::core::BlockType>(typeValue);
  if (type == agent::core::BlockType::ToolUse) {
    *block = agent::core::ContentBlock::MakeToolUse(toolUseId, toolName, toolInput);
  } else if (type == agent::core::BlockType::ToolResult) {
    *block = agent::core::ContentBlock::MakeToolResult(
        resultToolUseId, resultContent, resultIsError);
  } else {
    *block = agent::core::ContentBlock::MakeText(text);
  }
  return true;
}

std::string SerializeProtoMessage(const agent::core::Message& message) {
  std::string output;
  protolite::WriteInt32(&output, kMessageRole, static_cast<int>(message.role));
  protolite::WriteString(&output, kMessageUuid, message.uuid);
  protolite::WriteBool(&output, kMessageIsMeta, message.isMeta);
  protolite::WriteString(&output, kMessageStopReason, message.stopReason);
  protolite::WriteBool(
      &output, kMessageIsApiError, message.isApiErrorMessage);
  protolite::WriteMessage(&output, kMessageUsage, SerializeProtoUsage(message.usage));
  for (const auto& block : message.content) {
    protolite::WriteMessage(&output, kMessageBlock, SerializeProtoBlock(block));
  }
  return output;
}

bool DeserializeProtoMessage(
    const std::string& payload,
    agent::core::Message* message) {
  if (message == nullptr) {
    return false;
  }
  protolite::Reader reader(payload);
  protolite::Field field;
  while (reader.ReadField(&field)) {
    switch (field.number) {
      case kMessageRole: {
        int roleValue = 0;
        protolite::FieldToInt32(field, &roleValue);
        message->role = static_cast<agent::core::MessageRole>(roleValue);
        break;
      }
      case kMessageUuid:
        protolite::FieldToString(field, &message->uuid);
        break;
      case kMessageIsMeta:
        protolite::FieldToBool(field, &message->isMeta);
        break;
      case kMessageStopReason:
        protolite::FieldToString(field, &message->stopReason);
        break;
      case kMessageIsApiError:
        protolite::FieldToBool(field, &message->isApiErrorMessage);
        break;
      case kMessageUsage:
        if (!DeserializeProtoUsage(field.lengthDelimitedValue, &message->usage)) {
          return false;
        }
        break;
      case kMessageBlock: {
        agent::core::ContentBlock block;
        if (!DeserializeProtoBlock(field.lengthDelimitedValue, &block)) {
          return false;
        }
        message->content.push_back(block);
        break;
      }
      default:
        break;
    }
  }
  return reader.ok();
}

std::string SerializeProtoTaskDefinition(const agent::agents::SubAgentTask& task) {
  std::string output;
  protolite::WriteString(&output, kTaskPrompt, task.prompt);
  protolite::WriteString(&output, kTaskDescription, task.description);
  protolite::WriteString(&output, kTaskSubagentType, task.subagentType);
  protolite::WriteString(&output, kTaskModel, task.model);
  protolite::WriteInt32(&output, kTaskPriority, task.priority);
  protolite::WriteInt32(
      &output, kTaskRequiredSlots, task.requiredExecutorSlots);
  protolite::WriteBool(&output, kTaskRunInBackground, task.runInBackground);
  protolite::WriteString(&output, kTaskIsolation, task.isolation);
  protolite::WriteString(&output, kTaskCwd, task.cwd);
  protolite::WriteString(&output, kTaskParentCwd, task.parentCwd);
  protolite::WriteString(&output, kTaskWorktreeCwd, task.worktreeCwd);
  protolite::WriteString(&output, kTaskName, task.name);
  protolite::WriteString(&output, kTaskTeamName, task.teamName);
  protolite::WriteString(&output, kTaskMode, task.mode);
  return output;
}

bool DeserializeProtoTaskDefinition(
    const std::string& payload,
    agent::agents::SubAgentTask* task) {
  if (task == nullptr) {
    return false;
  }
  protolite::Reader reader(payload);
  protolite::Field field;
  while (reader.ReadField(&field)) {
    switch (field.number) {
      case kTaskPrompt:
        protolite::FieldToString(field, &task->prompt);
        break;
      case kTaskDescription:
        protolite::FieldToString(field, &task->description);
        break;
      case kTaskSubagentType:
        protolite::FieldToString(field, &task->subagentType);
        break;
      case kTaskModel:
        protolite::FieldToString(field, &task->model);
        break;
      case kTaskPriority:
        protolite::FieldToInt32(field, &task->priority);
        break;
      case kTaskRequiredSlots:
        protolite::FieldToInt32(field, &task->requiredExecutorSlots);
        break;
      case kTaskRunInBackground:
        protolite::FieldToBool(field, &task->runInBackground);
        break;
      case kTaskIsolation:
        protolite::FieldToString(field, &task->isolation);
        break;
      case kTaskCwd:
        protolite::FieldToString(field, &task->cwd);
        break;
      case kTaskParentCwd:
        protolite::FieldToString(field, &task->parentCwd);
        break;
      case kTaskWorktreeCwd:
        protolite::FieldToString(field, &task->worktreeCwd);
        break;
      case kTaskName:
        protolite::FieldToString(field, &task->name);
        break;
      case kTaskTeamName:
        protolite::FieldToString(field, &task->teamName);
        break;
      case kTaskMode:
        protolite::FieldToString(field, &task->mode);
        break;
      default:
        break;
    }
  }
  return reader.ok();
}

std::string SerializeProtoCheckpoint(
    const agent::agents::SubAgentTaskCheckpoint& checkpoint) {
  std::string output;
  protolite::WriteString(&output, kCheckpointId, checkpoint.checkpointId);
  protolite::WriteString(&output, kCheckpointCursor, checkpoint.resumeCursor);
  protolite::WriteInt64(&output, kCheckpointSavedAt, checkpoint.savedAtUnixMs);
  protolite::WriteBool(&output, kCheckpointResumable, checkpoint.resumable);
  return output;
}

bool DeserializeProtoCheckpoint(
    const std::string& payload,
    agent::agents::SubAgentTaskCheckpoint* checkpoint) {
  if (checkpoint == nullptr) {
    return false;
  }
  protolite::Reader reader(payload);
  protolite::Field field;
  while (reader.ReadField(&field)) {
    switch (field.number) {
      case kCheckpointId:
        protolite::FieldToString(field, &checkpoint->checkpointId);
        break;
      case kCheckpointCursor:
        protolite::FieldToString(field, &checkpoint->resumeCursor);
        break;
      case kCheckpointSavedAt:
        protolite::FieldToInt64(field, &checkpoint->savedAtUnixMs);
        break;
      case kCheckpointResumable:
        protolite::FieldToBool(field, &checkpoint->resumable);
        break;
      default:
        break;
    }
  }
  return reader.ok();
}

std::string SerializeProtoTaskLifecycle(
    const agent::agents::SubAgentTaskLifecycle& task) {
  std::string output;
  protolite::WriteString(&output, kTaskLifecycleId, task.taskId);
  protolite::WriteString(&output, kTaskLifecycleDirective, task.directive);
  protolite::WriteString(
      &output, kTaskLifecycleWorktreeNotice, task.worktreeNotice);
  protolite::WriteString(
      &output, kTaskLifecyclePlaceholder, task.placeholderResult);
  protolite::WriteString(&output, kTaskLifecycleSummary, task.summary);
  protolite::WriteInt32(&output, kTaskLifecycleState, static_cast<int>(task.state));
  protolite::WriteInt64(&output, kTaskLifecycleCreatedAt, task.createdAtUnixMs);
  protolite::WriteInt64(&output, kTaskLifecycleUpdatedAt, task.updatedAtUnixMs);
  protolite::WriteMessage(
      &output, kTaskLifecycleTask, SerializeProtoTaskDefinition(task.task));
  protolite::WriteString(
      &output, kTaskLifecycleAssignedExecutor, task.assignedExecutorId);
  protolite::WriteString(
      &output, kTaskLifecycleFailureReason, task.lastFailureReason);
  protolite::WriteInt32(&output, kTaskLifecycleAttemptCount, task.attemptCount);
  protolite::WriteMessage(
      &output, kTaskLifecycleCheckpoint, SerializeProtoCheckpoint(task.checkpoint));
  return output;
}

bool DeserializeProtoTaskLifecycle(
    const std::string& payload,
    agent::agents::SubAgentTaskLifecycle* task) {
  if (task == nullptr) {
    return false;
  }
  protolite::Reader reader(payload);
  protolite::Field field;
  while (reader.ReadField(&field)) {
    switch (field.number) {
      case kTaskLifecycleId:
        protolite::FieldToString(field, &task->taskId);
        break;
      case kTaskLifecycleDirective:
        protolite::FieldToString(field, &task->directive);
        break;
      case kTaskLifecycleWorktreeNotice:
        protolite::FieldToString(field, &task->worktreeNotice);
        break;
      case kTaskLifecyclePlaceholder:
        protolite::FieldToString(field, &task->placeholderResult);
        break;
      case kTaskLifecycleSummary:
        protolite::FieldToString(field, &task->summary);
        break;
      case kTaskLifecycleState: {
        int stateValue = 0;
        protolite::FieldToInt32(field, &stateValue);
        task->state = static_cast<agent::agents::SubAgentTaskState>(stateValue);
        break;
      }
      case kTaskLifecycleCreatedAt:
        protolite::FieldToInt64(field, &task->createdAtUnixMs);
        break;
      case kTaskLifecycleUpdatedAt:
        protolite::FieldToInt64(field, &task->updatedAtUnixMs);
        break;
      case kTaskLifecycleTask:
        if (!DeserializeProtoTaskDefinition(
                field.lengthDelimitedValue, &task->task)) {
          return false;
        }
        break;
      case kTaskLifecycleAssignedExecutor:
        protolite::FieldToString(field, &task->assignedExecutorId);
        break;
      case kTaskLifecycleFailureReason:
        protolite::FieldToString(field, &task->lastFailureReason);
        break;
      case kTaskLifecycleAttemptCount:
        protolite::FieldToInt32(field, &task->attemptCount);
        break;
      case kTaskLifecycleCheckpoint:
        if (!DeserializeProtoCheckpoint(
                field.lengthDelimitedValue, &task->checkpoint)) {
          return false;
        }
        break;
      default:
        break;
    }
  }
  return reader.ok();
}

std::string SerializeProtoExecutor(
    const agent::agents::SubAgentExecutorSlot& executor) {
  std::string output;
  protolite::WriteString(&output, kExecutorId, executor.executorId);
  protolite::WriteString(&output, kExecutorHost, executor.hostName);
  protolite::WriteInt32(&output, kExecutorMaxParallel, executor.maxParallelTasks);
  protolite::WriteInt32(&output, kExecutorRunning, executor.runningTasks);
  protolite::WriteInt32(&output, kExecutorWeight, executor.weight);
  protolite::WriteBool(&output, kExecutorHealthy, executor.healthy);
  protolite::WriteBool(
      &output, kExecutorCheckpointResume, executor.supportsCheckpointResume);
  protolite::WriteInt32(
      &output, kExecutorState, ExecutorStateToInt(executor.state));
  protolite::WriteInt64(
      &output, kExecutorLastHeartbeat, executor.lastHeartbeatUnixMs);
  protolite::WriteString(&output, kExecutorLastError, executor.lastError);
  return output;
}

bool DeserializeProtoExecutor(
    const std::string& payload,
    agent::agents::SubAgentExecutorSlot* executor) {
  if (executor == nullptr) {
    return false;
  }
  protolite::Reader reader(payload);
  protolite::Field field;
  while (reader.ReadField(&field)) {
    switch (field.number) {
      case kExecutorId:
        protolite::FieldToString(field, &executor->executorId);
        break;
      case kExecutorHost:
        protolite::FieldToString(field, &executor->hostName);
        break;
      case kExecutorMaxParallel:
        protolite::FieldToInt32(field, &executor->maxParallelTasks);
        break;
      case kExecutorRunning:
        protolite::FieldToInt32(field, &executor->runningTasks);
        break;
      case kExecutorWeight:
        protolite::FieldToInt32(field, &executor->weight);
        break;
      case kExecutorHealthy:
        protolite::FieldToBool(field, &executor->healthy);
        break;
      case kExecutorCheckpointResume:
        protolite::FieldToBool(field, &executor->supportsCheckpointResume);
        break;
      case kExecutorState: {
        int stateValue = 0;
        protolite::FieldToInt32(field, &stateValue);
        executor->state = ExecutorStateFromInt(stateValue);
        break;
      }
      case kExecutorLastHeartbeat:
        protolite::FieldToInt64(field, &executor->lastHeartbeatUnixMs);
        break;
      case kExecutorLastError:
        protolite::FieldToString(field, &executor->lastError);
        break;
      default:
        break;
    }
  }
  return reader.ok();
}

std::string SerializeProtoMetadata(const agent::core::SessionMetadata& metadata) {
  std::string output;
  protolite::WriteString(&output, kMetadataId, metadata.id);
  protolite::WriteInt32(&output, kMetadataTurnCount, metadata.turnCount);
  return output;
}

bool DeserializeProtoMetadata(
    const std::string& payload,
    agent::core::SessionMetadata* metadata) {
  if (metadata == nullptr) {
    return false;
  }
  protolite::Reader reader(payload);
  protolite::Field field;
  while (reader.ReadField(&field)) {
    switch (field.number) {
      case kMetadataId:
        protolite::FieldToString(field, &metadata->id);
        break;
      case kMetadataTurnCount:
        protolite::FieldToInt32(field, &metadata->turnCount);
        break;
      default:
        break;
    }
  }
  return reader.ok();
}

std::string SerializeProtoSnapshot(const agent::infra::SessionSnapshot& snapshot) {
  std::string output;
  protolite::WriteInt32(&output, kProtoFieldFormatVersion, snapshot.formatVersion);
  protolite::WriteString(&output, kProtoFieldSessionId, snapshot.sessionId);
  protolite::WriteString(&output, kProtoFieldTimestamp, snapshot.timestamp);
  protolite::WriteMessage(
      &output, kProtoFieldMetadata, SerializeProtoMetadata(snapshot.metadata));
  for (const auto& message : snapshot.messages) {
    protolite::WriteMessage(
        &output, kProtoFieldMessage, SerializeProtoMessage(message));
  }
  for (const auto& task : snapshot.subAgentTasks) {
    protolite::WriteMessage(
        &output, kProtoFieldSubTask, SerializeProtoTaskLifecycle(task));
  }
  for (const auto& executor : snapshot.subAgentExecutors) {
    protolite::WriteMessage(
        &output, kProtoFieldExecutor, SerializeProtoExecutor(executor));
  }
  return output;
}

bool DeserializeProtoSnapshot(
    const std::string& data,
    agent::infra::SessionSnapshot* snapshot) {
  if (snapshot == nullptr || data.empty()) {
    return false;
  }
  protolite::Reader reader(data);
  protolite::Field field;
  while (reader.ReadField(&field)) {
    switch (field.number) {
      case kProtoFieldFormatVersion:
        protolite::FieldToInt32(field, &snapshot->formatVersion);
        break;
      case kProtoFieldSessionId:
        protolite::FieldToString(field, &snapshot->sessionId);
        break;
      case kProtoFieldTimestamp:
        protolite::FieldToString(field, &snapshot->timestamp);
        break;
      case kProtoFieldMetadata:
        if (!DeserializeProtoMetadata(
                field.lengthDelimitedValue, &snapshot->metadata)) {
          return false;
        }
        break;
      case kProtoFieldMessage: {
        agent::core::Message message;
        if (!DeserializeProtoMessage(field.lengthDelimitedValue, &message)) {
          return false;
        }
        snapshot->messages.push_back(message);
        break;
      }
      case kProtoFieldSubTask: {
        agent::agents::SubAgentTaskLifecycle task;
        if (!DeserializeProtoTaskLifecycle(
                field.lengthDelimitedValue, &task)) {
          return false;
        }
        snapshot->subAgentTasks.push_back(task);
        break;
      }
      case kProtoFieldExecutor: {
        agent::agents::SubAgentExecutorSlot executor;
        if (!DeserializeProtoExecutor(
                field.lengthDelimitedValue, &executor)) {
          return false;
        }
        snapshot->subAgentExecutors.push_back(executor);
        break;
      }
      default:
        break;
    }
  }
  return reader.ok() && snapshot->formatVersion <= kSnapshotVersion;
}

std::string SerializeLegacyBinarySnapshot(
    const agent::infra::SessionSnapshot& snapshot) {
  std::string output;
  output.append(kLegacySnapshotMagic, kLegacySnapshotMagic + 7);
  AppendU16(&output, kLegacySnapshotVersion);
  AppendU16(&output, kLegacySnapshotReserved);
  AppendField(&output, kFieldSessionId, MakeStringFieldPayload(snapshot.sessionId));
  AppendField(&output, kFieldTimestamp, MakeStringFieldPayload(snapshot.timestamp));
  AppendField(&output, kFieldMetadata, SerializeMetadata(snapshot.metadata));
  for (const auto& message : snapshot.messages) {
    AppendField(&output, kFieldMessage, SerializeMessage(message));
  }
  for (const auto& task : snapshot.subAgentTasks) {
    AppendField(&output, kFieldSubTask, SerializeTaskLifecycle(task));
  }
  for (const auto& executor : snapshot.subAgentExecutors) {
    AppendField(&output, kFieldExecutor, SerializeExecutor(executor));
  }
  return output;
}

bool DeserializeLegacyBinarySnapshot(
    const std::string& data,
    agent::infra::SessionSnapshot* snapshot) {
  if (snapshot == nullptr || data.size() < 11 ||
      data.compare(0, 7, kLegacySnapshotMagic) != 0) {
    return false;
  }

  std::size_t cursor = 7;
  std::uint16_t version = 0;
  std::uint16_t reserved = 0;
  if (!ReadU16(data, &cursor, &version) || !ReadU16(data, &cursor, &reserved)) {
    return false;
  }
  if (version > kLegacySnapshotVersion) {
    return false;
  }
  snapshot->formatVersion = version;

  while (cursor < data.size()) {
    std::uint16_t tag = 0;
    std::string payload;
    if (!ReadNextField(data, &cursor, &tag, &payload)) {
      return false;
    }
    switch (tag) {
      case kFieldSessionId:
        snapshot->sessionId = payload;
        break;
      case kFieldTimestamp:
        snapshot->timestamp = payload;
        break;
      case kFieldMetadata:
        if (!DeserializeMetadata(payload, &snapshot->metadata)) {
          return false;
        }
        break;
      case kFieldMessage: {
        agent::core::Message message;
        if (!DeserializeMessage(payload, &message)) {
          return false;
        }
        snapshot->messages.push_back(message);
        break;
      }
      case kFieldSubTask: {
        agent::agents::SubAgentTaskLifecycle task;
        if (!DeserializeTaskLifecycle(payload, &task)) {
          return false;
        }
        snapshot->subAgentTasks.push_back(task);
        break;
      }
      case kFieldExecutor: {
        agent::agents::SubAgentExecutorSlot executor;
        if (!DeserializeExecutor(payload, &executor)) {
          return false;
        }
        snapshot->subAgentExecutors.push_back(executor);
        break;
      }
      default:
        break;
    }
  }
  return true;
}

void BuildLegacySnapshotText(
    const agent::infra::SessionSnapshot& snap,
    std::string* output) {
  std::ostringstream body;
  body << "=== SESSION SNAPSHOT ===\n";
  body << "session_id=" << snap.sessionId << "\n";
  body << "timestamp=" << snap.timestamp << "\n";
  body << "turn_count=" << snap.metadata.turnCount << "\n";
  body << "message_count=" << snap.messages.size() << "\n";
  body << "subtask_count=" << snap.subAgentTasks.size() << "\n";

  for (std::size_t i = 0; i < snap.messages.size(); ++i) {
    const auto& message = snap.messages[i];
    const std::string prefix = "message." + std::to_string(i) + ".";
    body << prefix << "role=" << RoleToString(message.role) << "\n";
    body << prefix << "uuid=" << EscapeField(message.uuid) << "\n";
    body << prefix << "is_meta=" << BoolToString(message.isMeta) << "\n";
    body << prefix << "stop_reason=" << EscapeField(message.stopReason) << "\n";
    body << prefix << "is_api_error=" << BoolToString(message.isApiErrorMessage)
         << "\n";
    body << prefix << "usage.input_tokens=" << message.usage.inputTokens << "\n";
    body << prefix << "usage.output_tokens=" << message.usage.outputTokens
         << "\n";
    body << prefix << "usage.cache_read=" << message.usage.cacheReadInputTokens
         << "\n";
    body << prefix << "usage.cache_create="
         << message.usage.cacheCreationInputTokens << "\n";
    body << prefix << "block_count=" << message.content.size() << "\n";

    for (std::size_t j = 0; j < message.content.size(); ++j) {
      const auto& block = message.content[j];
      const std::string blockPrefix =
          prefix + "block." + std::to_string(j) + ".";
      body << blockPrefix << "type=" << BlockTypeToString(block.type) << "\n";
      if (block.type == core::BlockType::Text) {
        body << blockPrefix << "text=" << EscapeField(block.asText.text) << "\n";
      } else if (block.type == core::BlockType::ToolUse) {
        body << blockPrefix << "tool_id="
             << EscapeField(block.asToolUse.id) << "\n";
        body << blockPrefix << "tool_name="
             << EscapeField(block.asToolUse.name) << "\n";
        body << blockPrefix << "tool_input="
             << EscapeField(block.asToolUse.inputJson) << "\n";
      } else if (block.type == core::BlockType::ToolResult) {
        body << blockPrefix << "tool_use_id="
             << EscapeField(block.asToolResult.toolUseId) << "\n";
        body << blockPrefix << "content="
             << EscapeField(block.asToolResult.content) << "\n";
        body << blockPrefix << "is_error="
             << BoolToString(block.asToolResult.isError) << "\n";
      }
    }
  }

  for (std::size_t i = 0; i < snap.subAgentTasks.size(); ++i) {
    const auto& task = snap.subAgentTasks[i];
    const std::string prefix = "subtask." + std::to_string(i) + ".";
    body << prefix << "task_id=" << EscapeField(task.taskId) << "\n";
    body << prefix << "directive=" << EscapeField(task.directive) << "\n";
    body << prefix << "worktree_notice=" << EscapeField(task.worktreeNotice)
         << "\n";
    body << prefix << "placeholder_result="
         << EscapeField(task.placeholderResult) << "\n";
    body << prefix << "summary=" << EscapeField(task.summary) << "\n";
    body << prefix << "state=" << TaskStateToString(task.state) << "\n";
    body << prefix << "created_at=" << task.createdAtUnixMs << "\n";
    body << prefix << "updated_at=" << task.updatedAtUnixMs << "\n";
    body << prefix << "task.prompt=" << EscapeField(task.task.prompt) << "\n";
    body << prefix << "task.description="
         << EscapeField(task.task.description) << "\n";
    body << prefix << "task.subagent_type="
         << EscapeField(task.task.subagentType) << "\n";
    body << prefix << "task.model=" << EscapeField(task.task.model) << "\n";
    body << prefix << "task.run_in_background="
         << BoolToString(task.task.runInBackground) << "\n";
    body << prefix << "task.isolation=" << EscapeField(task.task.isolation)
         << "\n";
    body << prefix << "task.cwd=" << EscapeField(task.task.cwd) << "\n";
    body << prefix << "task.parent_cwd="
         << EscapeField(task.task.parentCwd) << "\n";
    body << prefix << "task.worktree_cwd="
         << EscapeField(task.task.worktreeCwd) << "\n";
    body << prefix << "task.name=" << EscapeField(task.task.name) << "\n";
    body << prefix << "task.team_name=" << EscapeField(task.task.teamName)
         << "\n";
    body << prefix << "task.mode=" << EscapeField(task.task.mode) << "\n";
  }
  body << "=== END ===\n";
  *output = body.str();
}

bool RestoreLegacyTextSnapshot(
    const std::string& data,
    agent::infra::SessionSnapshot* snapshot) {
  if (snapshot == nullptr || data.empty()) {
    return false;
  }

  std::istringstream stream(data);
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty()) {
      lines.push_back(line);
    }
  }

  snapshot->sessionId = ReadSnapshotValue(lines, "session_id");
  snapshot->timestamp = ReadSnapshotValue(lines, "timestamp");
  snapshot->metadata.id = snapshot->sessionId;
  snapshot->metadata.turnCount = ReadSnapshotInt(lines, "turn_count");

  const int messageCount = ReadSnapshotInt(lines, "message_count");
  for (int i = 0; i < messageCount; ++i) {
    core::Message message;
    const std::string prefix = "message." + std::to_string(i) + ".";
    message.role = RoleFromString(ReadSnapshotValue(lines, prefix + "role"));
    message.uuid = ReadSnapshotValue(lines, prefix + "uuid");
    message.isMeta = ParseBool(ReadSnapshotValue(lines, prefix + "is_meta"));
    message.stopReason = ReadSnapshotValue(lines, prefix + "stop_reason");
    message.isApiErrorMessage =
        ParseBool(ReadSnapshotValue(lines, prefix + "is_api_error"));
    message.usage.inputTokens =
        ReadSnapshotInt(lines, prefix + "usage.input_tokens");
    message.usage.outputTokens =
        ReadSnapshotInt(lines, prefix + "usage.output_tokens");
    message.usage.cacheReadInputTokens =
        ReadSnapshotInt(lines, prefix + "usage.cache_read");
    message.usage.cacheCreationInputTokens =
        ReadSnapshotInt(lines, prefix + "usage.cache_create");

    const int blockCount = ReadSnapshotInt(lines, prefix + "block_count");
    for (int j = 0; j < blockCount; ++j) {
      const std::string blockPrefix =
          prefix + "block." + std::to_string(j) + ".";
      const core::BlockType type =
          BlockTypeFromString(ReadSnapshotValue(lines, blockPrefix + "type"));
      if (type == core::BlockType::Text) {
        message.content.push_back(core::ContentBlock::MakeText(
            ReadSnapshotValue(lines, blockPrefix + "text")));
      } else if (type == core::BlockType::ToolUse) {
        message.content.push_back(core::ContentBlock::MakeToolUse(
            ReadSnapshotValue(lines, blockPrefix + "tool_id"),
            ReadSnapshotValue(lines, blockPrefix + "tool_name"),
            ReadSnapshotValue(lines, blockPrefix + "tool_input")));
      } else if (type == core::BlockType::ToolResult) {
        message.content.push_back(core::ContentBlock::MakeToolResult(
            ReadSnapshotValue(lines, blockPrefix + "tool_use_id"),
            ReadSnapshotValue(lines, blockPrefix + "content"),
            ParseBool(ReadSnapshotValue(lines, blockPrefix + "is_error"))));
      }
    }
    snapshot->messages.push_back(message);
  }

  const int taskCount = ReadSnapshotInt(lines, "subtask_count");
  for (int i = 0; i < taskCount; ++i) {
    agents::SubAgentTaskLifecycle task;
    const std::string prefix = "subtask." + std::to_string(i) + ".";
    task.taskId = ReadSnapshotValue(lines, prefix + "task_id");
    task.directive = ReadSnapshotValue(lines, prefix + "directive");
    task.worktreeNotice = ReadSnapshotValue(lines, prefix + "worktree_notice");
    task.placeholderResult =
        ReadSnapshotValue(lines, prefix + "placeholder_result");
    task.summary = ReadSnapshotValue(lines, prefix + "summary");
    task.state = TaskStateFromString(ReadSnapshotValue(lines, prefix + "state"));
    task.createdAtUnixMs = ReadSnapshotLongLong(lines, prefix + "created_at");
    task.updatedAtUnixMs = ReadSnapshotLongLong(lines, prefix + "updated_at");
    task.task.prompt = ReadSnapshotValue(lines, prefix + "task.prompt");
    task.task.description =
        ReadSnapshotValue(lines, prefix + "task.description");
    task.task.subagentType =
        ReadSnapshotValue(lines, prefix + "task.subagent_type");
    task.task.model = ReadSnapshotValue(lines, prefix + "task.model");
    task.task.runInBackground =
        ParseBool(ReadSnapshotValue(lines, prefix + "task.run_in_background"));
    task.task.isolation = ReadSnapshotValue(lines, prefix + "task.isolation");
    task.task.cwd = ReadSnapshotValue(lines, prefix + "task.cwd");
    task.task.parentCwd = ReadSnapshotValue(lines, prefix + "task.parent_cwd");
    task.task.worktreeCwd =
        ReadSnapshotValue(lines, prefix + "task.worktree_cwd");
    task.task.name = ReadSnapshotValue(lines, prefix + "task.name");
    task.task.teamName = ReadSnapshotValue(lines, prefix + "task.team_name");
    task.task.mode = ReadSnapshotValue(lines, prefix + "task.mode");
    snapshot->subAgentTasks.push_back(task);
  }
  return true;
}

}  // namespace

SessionManager::SessionManager(const std::string& sessionDir)
    : sessionDir_(sessionDir) {}

void SessionManager::SetMessages(const std::vector<core::Message>& messages) {
  std::lock_guard<std::mutex> lock(mutex_);
  messages_ = messages;
}

std::vector<core::Message> SessionManager::messages() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return messages_;
}

void SessionManager::AppendMessage(const core::Message& message) {
  std::lock_guard<std::mutex> lock(mutex_);
  messages_.push_back(message);
}

void SessionManager::SetMetadata(const core::SessionMetadata& metadata) {
  std::lock_guard<std::mutex> lock(mutex_);
  metadata_ = metadata;
}

core::SessionMetadata SessionManager::metadata() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return metadata_;
}

void SessionManager::SetSubAgentTasks(
    const std::vector<agents::SubAgentTaskLifecycle>& subAgentTasks) {
  std::lock_guard<std::mutex> lock(mutex_);
  subAgentTasks_ = subAgentTasks;
}

void SessionManager::UpsertSubAgentTask(
    const agents::SubAgentTaskLifecycle& task) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& existing : subAgentTasks_) {
    if (existing.taskId == task.taskId) {
      existing = task;
      return;
    }
  }
  subAgentTasks_.push_back(task);
}

std::vector<agents::SubAgentTaskLifecycle> SessionManager::subAgentTasks() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return subAgentTasks_;
}

void SessionManager::SetSubAgentExecutors(
    const std::vector<agents::SubAgentExecutorSlot>& executors) {
  std::lock_guard<std::mutex> lock(mutex_);
  subAgentExecutors_ = executors;
}

std::vector<agents::SubAgentExecutorSlot> SessionManager::subAgentExecutors() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return subAgentExecutors_;
}

SessionSnapshot SessionManager::BuildSnapshot() const {
  SessionSnapshot snap;
  snap.formatVersion = kSnapshotVersion;
  snap.sessionId = metadata_.id;
  snap.timestamp = TimestampNow();
  snap.messages = messages_;
  snap.subAgentTasks = subAgentTasks_;
  snap.subAgentExecutors = subAgentExecutors_;
  snap.metadata = metadata_;
  return snap;
}

void SessionManager::PersistSnapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  const SessionSnapshot snap = BuildSnapshot();

  WriteBinaryFile(SnapshotPath(), SerializeProtoSnapshot(snap));

  std::string legacyText;
  BuildLegacySnapshotText(snap, &legacyText);
  WriteTextFile(LegacySnapshotPath(), legacyText);

  std::ostringstream transcript;
  for (const auto& m : snap.messages) {
    const char* roleStr = "unknown";
    if (m.role == core::MessageRole::User) roleStr = "user";
    else if (m.role == core::MessageRole::Assistant) roleStr = "asst";
    else if (m.role == core::MessageRole::System) roleStr = "sys";
    transcript << "[" << roleStr << "] blocks=" << m.content.size() << "\n";

    for (const auto& b : m.content) {
      if (b.type == core::BlockType::Text) {
        transcript << "  text: " << b.asText.text << "\n";
      } else if (b.type == core::BlockType::ToolUse) {
        transcript << "  tool_use: " << b.asToolUse.name
                   << " id=" << b.asToolUse.id << "\n";
      } else if (b.type == core::BlockType::ToolResult) {
        transcript << "  tool_result: id=" << b.asToolResult.toolUseId
                   << " isError=" << b.asToolResult.isError << "\n";
      }
    }
  }

  WriteTextFile(LatestTranscriptPath(), transcript.str());
}

bool SessionManager::RestoreFromDisk() {
  SessionSnapshot restored;
  const std::string binary = ReadTextFile(SnapshotPath());
  bool restoredOk = false;
  if (!binary.empty()) {
    restoredOk = DeserializeProtoSnapshot(binary, &restored);
  }
  if (!restoredOk) {
    const std::string legacyBinary = ReadTextFile(LegacyBinarySnapshotPath());
    if (!legacyBinary.empty()) {
      restoredOk = DeserializeLegacyBinarySnapshot(legacyBinary, &restored);
    }
  }
  if (!restoredOk) {
    const std::string legacy = ReadTextFile(LegacySnapshotPath());
    if (legacy.empty() || !RestoreLegacyTextSnapshot(legacy, &restored)) {
      return false;
    }
  }

  std::lock_guard<std::mutex> lock(mutex_);
  metadata_ = restored.metadata;
  if (metadata_.id.empty()) {
    metadata_.id = restored.sessionId;
  }
  messages_ = restored.messages;
  subAgentTasks_ = restored.subAgentTasks;
  subAgentExecutors_ = restored.subAgentExecutors;
  return true;
}

std::string SessionManager::LatestTranscriptPath() const {
  return sessionDir_ + "\\transcript.txt";
}

std::string SessionManager::TranscriptJsonlPath() const {
  return sessionDir_ + "\\transcript.jsonl";
}

std::string SessionManager::MainModelIoPath() const {
  return sessionDir_ + "\\main-model.jsonl";
}

std::string SessionManager::ValidatorModelIoPath() const {
  return sessionDir_ + "\\validator-model.jsonl";
}

std::string SessionManager::SnapshotPath() const {
  return sessionDir_ + "\\snapshot.pb";
}

std::string SessionManager::LegacyBinarySnapshotPath() const {
  return sessionDir_ + "\\snapshot.bin";
}

std::string SessionManager::LegacySnapshotPath() const {
  return sessionDir_ + "\\snapshot.txt";
}

namespace {

using json = nlohmann::json;

std::string BlockTypeToJsonStr(core::BlockType type) {
  switch (type) {
    case core::BlockType::Text: return "text";
    case core::BlockType::ToolUse: return "tool_use";
    case core::BlockType::ToolResult: return "tool_result";
  }
  return "text";
}

std::string RoleToJsonStr(core::MessageRole role) {
  switch (role) {
    case core::MessageRole::User: return "user";
    case core::MessageRole::Assistant: return "assistant";
    case core::MessageRole::System: return "system";
  }
  return "user";
}

json MessageToJson(const core::Message& msg) {
  json j;
  j["role"] = RoleToJsonStr(msg.role);
  j["uuid"] = msg.uuid;
  j["isMeta"] = msg.isMeta;
  if (msg.isApiErrorMessage) j["isApiErrorMessage"] = true;

  json content = json::array();
  for (const auto& block : msg.content) {
    json b;
    b["type"] = BlockTypeToJsonStr(block.type);
    if (block.type == core::BlockType::Text) {
      b["text"] = block.asText.text;
    } else if (block.type == core::BlockType::ToolUse) {
      b["id"] = block.asToolUse.id;
      b["name"] = block.asToolUse.name;
      try {
        b["input"] = json::parse(block.asToolUse.inputJson);
      } catch (...) {
        b["input"] = json::object();
      }
    } else if (block.type == core::BlockType::ToolResult) {
      b["tool_use_id"] = block.asToolResult.toolUseId;
      b["content"] = block.asToolResult.content;
      if (block.asToolResult.isError) b["is_error"] = true;
    } else if (block.type == core::BlockType::Image) {
      b["media_type"] = block.asImage.mediaType;
      b["base64_size"] = static_cast<int>(block.asImage.base64Data.size());
    }
    content.push_back(b);
  }
  j["content"] = content;
  if (!msg.stopReason.empty()) j["stop_reason"] = msg.stopReason;
  if (msg.usage.inputTokens > 0) {
    j["usage"] = {
      {"input_tokens", msg.usage.inputTokens},
      {"output_tokens", msg.usage.outputTokens},
      {"cache_read_input_tokens", msg.usage.cacheReadInputTokens},
      {"cache_creation_input_tokens", msg.usage.cacheCreationInputTokens},
    };
  }
  return j;
}

json MessagesToJson(const std::vector<core::Message>& messages) {
  json array = json::array();
  for (const auto& message : messages) {
    array.push_back(MessageToJson(message));
  }
  return array;
}

std::string MessageToJsonl(const core::Message& msg) {
  json j;
  j["type"] = RoleToJsonStr(msg.role);
  j["message"] = MessageToJson(msg);
  return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

}  // namespace

void SessionManager::AppendTranscriptLine(const std::string& jsonLine) {
  std::lock_guard<std::mutex> lock(transcriptMutex_);
  transcriptBuffer_.push_back(jsonLine);
  if (transcriptBuffer_.size() >= 50) {
    FlushTranscriptBuffer();
  }
}

void SessionManager::FlushTranscriptBuffer() {
  std::lock_guard<std::mutex> lock(transcriptMutex_);
  if (transcriptBuffer_.empty()) return;

  std::string path = TranscriptJsonlPath();
  EnsureDirectoryRecursive(sessionDir_);
  std::ofstream out(path, std::ios::app | std::ios::binary);
  if (!out) return;

  for (const auto& line : transcriptBuffer_) {
    out << line << "\n";
  }
  transcriptBuffer_.clear();
}

void SessionManager::AppendMessageToTranscript(const core::Message& msg) {
  AppendTranscriptLine(MessageToJsonl(msg));
}

void SessionManager::AppendModelIoRecord(
    ModelIoLogKind kind,
    const std::string& phase,
    const std::string& model,
    const std::string& systemPrompt,
    const std::vector<core::Message>& messages,
    int turnCount,
    const std::string& error) {
  json record;
  record["phase"] = phase;
  record["model"] = model;
  record["turn"] = turnCount;
  if (!systemPrompt.empty()) {
    record["systemPrompt"] = systemPrompt;
  }
  record["messages"] = MessagesToJson(messages);
  if (!error.empty()) {
    record["error"] = error;
  }

  const std::string path =
      kind == ModelIoLogKind::Main ? MainModelIoPath() : ValidatorModelIoPath();
  const std::string line =
      record.dump(-1, ' ', false, json::error_handler_t::replace);

  std::lock_guard<std::mutex> lock(transcriptMutex_);
  EnsureDirectoryRecursive(sessionDir_);
  std::ofstream out(path, std::ios::app | std::ios::binary);
  if (!out) return;
  out << line << "\n";
}

}  // namespace infra
}  // namespace agent
