#pragma once

// Structured logging system for cpp-agent.
// Replaces ad-hoc std::cout/std::cerr debug output and HTTP debug telemetry
// with a proper logger supporting levels, categories, file rotation, and
// an in-memory ring buffer for diagnostics.
//
// Usage:
//   LOG_INFO(MODEL, "calling model", {{"model", modelName}, {"tokens", "1234"}});
//   LOG_ERROR(TOOL, "tool execution failed", {{"tool", "Bash"}, {"error", msg}});
//   LOG_TRACE(QUERY, "state transition", {{"from", "ModelCall"}, {"to", "Validator"}});
//
// Environment variables:
//   CPP_AGENT_LOG_LEVEL    - minimum log level: TRACE|DEBUG|INFO|WARN|ERROR|FATAL
//                            (default: INFO)
//   CPP_AGENT_LOG_FILE     - custom log file path (default: logs/agent-YYYYMMDD.log)
//   CPP_AGENT_LOG_CONSOLE  - "1" to also write to console, "0" to suppress
//   CPP_AGENT_LOG_MAX_SIZE_MB - max log file size before rotation (default: 50)
//   CPP_AGENT_LOG_MAX_FILES  - number of rotated log files to keep (default: 5)

#include <chrono>
#include <cstddef>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace agent {
namespace infra {

// ---------- Log levels ----------

enum class LogLevel {
  TRACE = 0,
  DEBUG = 1,
  INFO = 2,
  WARN = 3,
  ERR = 4,    // named ERR to avoid conflict with Windows ERROR macro
  FATAL = 5,
};

const char* LogLevelToString(LogLevel level);
LogLevel LogLevelFromString(const std::string& str);

// ---------- Log categories ----------

enum class LogCategory {
  GENERAL,   // uncategorized
  MODEL,     // LLM API calls, SSE parsing, response handling
  QUERY,     // QueryLoop state machine, turn management
  TOOL,      // Tool dispatch, execution, results
  COMPACT,   // Auto-compact, micro-compact, context collapse
  PERM,      // Permission checks, sandbox enforcement
  SESSION,   // Session persistence, snapshot/restore
  TUI,       // Terminal UI rendering
  NET,       // Network I/O (HTTP, MCP connections)
  MEM,       // Memory system (memory index, session memory, autodream)
  INFRA,     // ThreadPool, ProcessRunner, StabilityWatchdog
  HOOK,      // Pre/post tool hooks
  SUBAGENT,  // Sub-agent spawning and coordination
};

const char* LogCategoryToString(LogCategory cat);

// ---------- Log entry ----------

struct LogEntry {
  LogLevel level = LogLevel::INFO;
  LogCategory category = LogCategory::GENERAL;
  std::string message;
  std::string timestamp;  // ISO 8601 with milliseconds
  std::string traceId;    // optional correlation ID for turn-level tracing
  std::vector<std::pair<std::string, std::string>> fields;  // structured KV pairs
  std::string file;       // source file (optional)
  int line = 0;           // source line (optional)
};

// ---------- Logger configuration ----------

struct LoggerConfig {
  LogLevel minLevel = LogLevel::INFO;
  std::string logFilePath;          // empty = auto-generate (logs/agent-YYYYMMDD.log)
  bool consoleOutput = true;
  bool fileOutput = true;
  std::size_t maxFileSizeBytes = 50 * 1024 * 1024;  // 50 MB
  int maxRotatedFiles = 5;
  std::size_t ringBufferSize = 1000;  // in-memory ring buffer entries
};

// ---------- Logger singleton ----------

class Logger {
 public:
  static Logger& Instance();

  // Initialize with config. Call once at startup.
  // If not called, defaults are used (level=INFO, console=true, file=true).
  void Init(const LoggerConfig& config);

  // Initialize from environment variables (CPP_AGENT_LOG_LEVEL etc.).
  void InitFromEnv();

  // Write a log entry.
  void Log(const LogEntry& entry);

  // Convenience: write a simple message.
  void Log(LogLevel level, LogCategory category, const std::string& message,
           const std::vector<std::pair<std::string, std::string>>& fields = {},
           const char* file = nullptr, int line = 0);

  // Set/get the current trace ID (for correlating events within a turn).
  void SetTraceId(const std::string& traceId);
  std::string GetTraceId() const;

  // Retrieve in-memory ring buffer entries (most recent first).
  std::vector<LogEntry> GetRecentEntries(std::size_t maxCount = 100) const;

  // Retrieve only error-level and above entries from ring buffer.
  std::vector<LogEntry> GetRecentErrors(std::size_t maxCount = 100) const;

  // Flush and close the log file.
  void Shutdown();

  // Get current configuration.
  const LoggerConfig& GetConfig() const;

  // Check if a level is enabled.
  bool IsLevelEnabled(LogLevel level) const;

 private:
  Logger();
  ~Logger();
  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;

  void EnsureFileOpen();
  void RotateFileIfNeeded();
  void WriteToConsole(const LogEntry& entry);
  void WriteToFile(const LogEntry& entry);
  void AddToRingBuffer(const LogEntry& entry);
  std::string FormatEntry(const LogEntry& entry) const;
  std::string FormatEntryColored(const LogEntry& entry) const;
  std::string GenerateTimestamp() const;
  std::string GenerateDefaultLogPath() const;

  LoggerConfig config_;
  mutable std::mutex mutex_;
  std::ofstream fileStream_;
  std::string currentFilePath_;
  std::size_t currentFileSize_ = 0;
  std::string traceId_;
  std::deque<LogEntry> ringBuffer_;
  bool initialized_ = false;
};

}  // namespace infra
}  // namespace agent

// ---------- Convenience macros ----------

#define LOG_TRACE(cat, msg, ...) \
  ::agent::infra::Logger::Instance().Log( \
      ::agent::infra::LogLevel::TRACE, \
      ::agent::infra::LogCategory::cat, msg, \
      {__VA_ARGS__}, __FILE__, __LINE__)

#define LOG_DEBUG(cat, msg, ...) \
  ::agent::infra::Logger::Instance().Log( \
      ::agent::infra::LogLevel::DEBUG, \
      ::agent::infra::LogCategory::cat, msg, \
      {__VA_ARGS__}, __FILE__, __LINE__)

#define LOG_INFO(cat, msg, ...) \
  ::agent::infra::Logger::Instance().Log( \
      ::agent::infra::LogLevel::INFO, \
      ::agent::infra::LogCategory::cat, msg, \
      {__VA_ARGS__}, __FILE__, __LINE__)

#define LOG_WARN(cat, msg, ...) \
  ::agent::infra::Logger::Instance().Log( \
      ::agent::infra::LogLevel::WARN, \
      ::agent::infra::LogCategory::cat, msg, \
      {__VA_ARGS__}, __FILE__, __LINE__)

#define LOG_ERROR(cat, msg, ...) \
  ::agent::infra::Logger::Instance().Log( \
      ::agent::infra::LogLevel::ERR, \
      ::agent::infra::LogCategory::cat, msg, \
      {__VA_ARGS__}, __FILE__, __LINE__)

#define LOG_FATAL(cat, msg, ...) \
  ::agent::infra::Logger::Instance().Log( \
      ::agent::infra::LogLevel::FATAL, \
      ::agent::infra::LogCategory::cat, msg, \
      {__VA_ARGS__}, __FILE__, __LINE__)
