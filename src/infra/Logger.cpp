#include "infra/Logger.h"
#include "infra/EnvUtil.h"
#include "infra/StringUtil.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
// Windows headers define toupper/tolower as macros, which conflicts
// with std::toupper/std::tolower used below.
#undef toupper
#undef tolower
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace agent {
namespace infra {

// =====================================================================
// Level / category string conversions
// =====================================================================

const char* LogLevelToString(LogLevel level) {
  switch (level) {
    case LogLevel::TRACE: return "TRACE";
    case LogLevel::DEBUG: return "DEBUG";
    case LogLevel::INFO:  return "INFO ";
    case LogLevel::WARN:  return "WARN ";
    case LogLevel::ERR:   return "ERROR";
    case LogLevel::FATAL: return "FATAL";
  }
  return "UNKNOWN";
}

LogLevel LogLevelFromString(const std::string& str) {
  std::string upper;
  for (char ch : str) upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
  if (upper == "TRACE") return LogLevel::TRACE;
  if (upper == "DEBUG") return LogLevel::DEBUG;
  if (upper == "INFO")  return LogLevel::INFO;
  if (upper == "WARN" || upper == "WARNING") return LogLevel::WARN;
  if (upper == "ERROR") return LogLevel::ERR;
  if (upper == "FATAL") return LogLevel::FATAL;
  return LogLevel::INFO;  // safe default
}

const char* LogCategoryToString(LogCategory cat) {
  switch (cat) {
    case LogCategory::GENERAL:  return "GEN";
    case LogCategory::MODEL:    return "MDL";
    case LogCategory::QUERY:    return "QRY";
    case LogCategory::TOOL:     return "TOL";
    case LogCategory::COMPACT:  return "CMP";
    case LogCategory::PERM:     return "PRM";
    case LogCategory::SESSION:  return "SES";
    case LogCategory::TUI:      return "TUI";
    case LogCategory::NET:      return "NET";
    case LogCategory::MEM:      return "MEM";
    case LogCategory::INFRA:    return "INF";
    case LogCategory::HOOK:     return "HK";
    case LogCategory::SUBAGENT: return "SUB";
  }
  return "???";
}

// =====================================================================
// Logger implementation
// =====================================================================

Logger::Logger() {}

Logger::~Logger() {
  Shutdown();
}

Logger& Logger::Instance() {
  static Logger instance;
  return instance;
}

void Logger::Init(const LoggerConfig& config) {
  std::lock_guard<std::mutex> lock(mutex_);
  config_ = config;
  if (config_.logFilePath.empty()) {
    config_.logFilePath = GenerateDefaultLogPath();
  }
  initialized_ = true;
  if (config_.fileOutput) {
    EnsureFileOpen();
  }
}

void Logger::InitFromEnv() {
  LoggerConfig config;
  config.minLevel = LogLevelFromString(GetEnvOrDefault("CPP_AGENT_LOG_LEVEL", "INFO"));
  config.logFilePath = GetEnvString("CPP_AGENT_LOG_FILE");
  // Default console output to false when running inside a TUI to avoid
  // corrupting the display; explicit CPP_AGENT_LOG_CONSOLE=1 overrides.
  const std::string consoleEnv = GetEnvString("CPP_AGENT_LOG_CONSOLE");
  if (consoleEnv.empty()) {
    config.consoleOutput = false;  // TUI manages console output
  } else {
    config.consoleOutput = IsTruthyEnvValue(consoleEnv);
  }
  config.maxFileSizeBytes =
      static_cast<std::size_t>(GetEnvInt("CPP_AGENT_LOG_MAX_SIZE_MB", 50)) * 1024 * 1024;
  config.maxRotatedFiles = GetEnvInt("CPP_AGENT_LOG_MAX_FILES", 5);
  Init(config);
}

void Logger::Log(const LogEntry& entry) {
  if (entry.level < config_.minLevel) return;
  std::lock_guard<std::mutex> lock(mutex_);
  if (config_.fileOutput) {
    EnsureFileOpen();
    WriteToFile(entry);
  }
  if (config_.consoleOutput) {
    WriteToConsole(entry);
  }
  AddToRingBuffer(entry);
}

void Logger::Log(LogLevel level, LogCategory category, const std::string& message,
                 const std::vector<std::pair<std::string, std::string>>& fields,
                 const char* file, int line) {
  if (level < config_.minLevel) return;
  LogEntry entry;
  entry.level = level;
  entry.category = category;
  entry.message = message;
  entry.timestamp = GenerateTimestamp();
  entry.fields = fields;
  if (file) entry.file = file;
  entry.line = line;
  // Inject current trace ID
  {
    std::lock_guard<std::mutex> lock(mutex_);
    entry.traceId = traceId_;
  }
  Log(entry);
}

void Logger::SetTraceId(const std::string& traceId) {
  std::lock_guard<std::mutex> lock(mutex_);
  traceId_ = traceId;
}

std::string Logger::GetTraceId() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return traceId_;
}

std::vector<LogEntry> Logger::GetRecentEntries(std::size_t maxCount) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<LogEntry> result;
  std::size_t start = ringBuffer_.size() > maxCount
                          ? ringBuffer_.size() - maxCount : 0;
  for (std::size_t i = start; i < ringBuffer_.size(); ++i) {
    result.push_back(ringBuffer_[i]);
  }
  // Reverse so most recent is first
  std::reverse(result.begin(), result.end());
  return result;
}

std::vector<LogEntry> Logger::GetRecentErrors(std::size_t maxCount) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<LogEntry> result;
  for (auto it = ringBuffer_.rbegin(); it != ringBuffer_.rend() && result.size() < maxCount; ++it) {
    if (it->level >= LogLevel::ERR) {
      result.push_back(*it);
    }
  }
  return result;
}

void Logger::Shutdown() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (fileStream_.is_open()) {
    fileStream_.flush();
    fileStream_.close();
  }
}

const LoggerConfig& Logger::GetConfig() const {
  return config_;
}

bool Logger::IsLevelEnabled(LogLevel level) const {
  return level >= config_.minLevel;
}

// =====================================================================
// Private methods
// =====================================================================

void Logger::EnsureFileOpen() {
  if (fileStream_.is_open()) return;
  if (currentFilePath_.empty()) {
    currentFilePath_ = config_.logFilePath;
  }

  // Ensure parent directory exists
  const std::string parent = ParentPath(currentFilePath_);
  if (!parent.empty()) {
#ifdef _WIN32
    // Recursive mkdir using Win32
    auto ensureDir = [](const std::string& path) {
      std::wstring wide = Utf8ToWide(path);
      DWORD attrs = GetFileAttributesW(wide.c_str());
      if (attrs != INVALID_FILE_ATTRIBUTES &&
          (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) return;
      CreateDirectoryW(wide.c_str(), nullptr);
    };
    // Simple: try to create; if parent doesn't exist, create it first
    std::wstring wideParent = Utf8ToWide(parent);
    DWORD attrs = GetFileAttributesW(wideParent.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES ||
        (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      std::string grandParent = ParentPath(parent);
      if (!grandParent.empty() && grandParent != parent) {
        std::wstring wideGP = Utf8ToWide(grandParent);
        DWORD gpAttrs = GetFileAttributesW(wideGP.c_str());
        if (gpAttrs == INVALID_FILE_ATTRIBUTES ||
            (gpAttrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
          CreateDirectoryW(wideGP.c_str(), nullptr);
        }
      }
      CreateDirectoryW(wideParent.c_str(), nullptr);
    }
#else
    mkdir(parent.c_str(), 0755);
#endif
  }

  fileStream_.open(currentFilePath_, std::ios::app | std::ios::binary);
  if (fileStream_.is_open()) {
    // Get current file size for rotation tracking
    fileStream_.seekp(0, std::ios::end);
    currentFileSize_ = static_cast<std::size_t>(fileStream_.tellp());
  }
}

void Logger::RotateFileIfNeeded() {
  if (currentFileSize_ < config_.maxFileSizeBytes) return;

  fileStream_.close();

  // Rotate: agent.log -> agent.log.1, agent.log.1 -> agent.log.2, etc.
  for (int i = config_.maxRotatedFiles - 1; i >= 1; --i) {
    std::string src = currentFilePath_ + "." + std::to_string(i);
    std::string dst = currentFilePath_ + "." + std::to_string(i + 1);
#ifdef _WIN32
    DeleteFileW(Utf8ToWide(dst).c_str());
    MoveFileW(Utf8ToWide(src).c_str(), Utf8ToWide(dst).c_str());
#else
    std::rename(src.c_str(), dst.c_str());
#endif
  }

  // Current -> .1
  std::string first = currentFilePath_ + ".1";
#ifdef _WIN32
  DeleteFileW(Utf8ToWide(first).c_str());
  MoveFileW(Utf8ToWide(currentFilePath_).c_str(), Utf8ToWide(first).c_str());
#else
  std::rename(currentFilePath_.c_str(), first.c_str());
#endif

  currentFileSize_ = 0;
  EnsureFileOpen();
}

void Logger::WriteToConsole(const LogEntry& entry) {
  std::cout << FormatEntryColored(entry) << "\n";
  std::cout.flush();
}

void Logger::WriteToFile(const LogEntry& entry) {
  const std::string formatted = FormatEntry(entry);
  fileStream_ << formatted << "\n";
  fileStream_.flush();
  currentFileSize_ += formatted.size() + 1;
  RotateFileIfNeeded();
}

void Logger::AddToRingBuffer(const LogEntry& entry) {
  ringBuffer_.push_back(entry);
  while (ringBuffer_.size() > config_.ringBufferSize) {
    ringBuffer_.pop_front();
  }
}

std::string Logger::FormatEntry(const LogEntry& entry) const {
  std::ostringstream oss;
  // ISO 8601 timestamp with milliseconds
  oss << entry.timestamp
      << " [" << LogLevelToString(entry.level) << "] "
      << "[" << LogCategoryToString(entry.category) << "] ";
  if (!entry.traceId.empty()) {
    oss << "[" << entry.traceId << "] ";
  }
  oss << entry.message;
  // Structured fields
  for (const auto& kv : entry.fields) {
    oss << " " << kv.first << "=" << kv.second;
  }
  // Source location (only for DEBUG and below)
  if (entry.level <= LogLevel::DEBUG && !entry.file.empty()) {
    oss << " (" << entry.file << ":" << entry.line << ")";
  }
  return oss.str();
}

std::string Logger::FormatEntryColored(const LogEntry& entry) const {
  // ANSI color codes for console output
  const char* colorStart = "";
  const char* colorEnd = "\x1b[0m";
  switch (entry.level) {
    case LogLevel::TRACE: colorStart = "\x1b[90m";    break;  // dim gray
    case LogLevel::DEBUG: colorStart = "\x1b[36m";    break;  // cyan
    case LogLevel::INFO:  colorStart = "\x1b[32m";    break;  // green
    case LogLevel::WARN:  colorStart = "\x1b[33m";    break;  // yellow
    case LogLevel::ERR: colorStart = "\x1b[31m";    break;  // red
    case LogLevel::FATAL: colorStart = "\x1b[1;31m";  break;  // bold red
  }

  std::ostringstream oss;
  oss << colorStart << entry.timestamp
      << " [" << LogLevelToString(entry.level) << "] "
      << "[" << LogCategoryToString(entry.category) << "] "
      << entry.message;
  for (const auto& kv : entry.fields) {
    oss << " " << kv.first << "=" << kv.second;
  }
  oss << colorEnd;
  return oss.str();
}

std::string Logger::GenerateTimestamp() const {
  auto now = std::chrono::system_clock::now();
  auto timeT = std::chrono::system_clock::to_time_t(now);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()) % 1000;
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &timeT);
#else
  localtime_r(&timeT, &tm);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S")
      << "." << std::setfill('0') << std::setw(3) << ms.count();
  return oss.str();
}

std::string Logger::GenerateDefaultLogPath() const {
  auto now = std::chrono::system_clock::now();
  auto timeT = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &timeT);
#else
  localtime_r(&timeT, &tm);
#endif
  std::ostringstream oss;
  oss << "logs"
#ifdef _WIN32
      << "\\"
#else
      << "/"
#endif
      << "agent-" << std::put_time(&tm, "%Y%m%d") << ".log";
  return oss.str();
}

}  // namespace infra
}  // namespace agent
