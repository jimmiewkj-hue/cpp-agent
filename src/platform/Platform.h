#pragma once

// Platform abstraction layer for cpp-agent.
// Isolates all OS-specific code (WinHTTP, Win32 file I/O, CreateProcessW)
// behind a unified API so the rest of the codebase can compile on both
// Windows and Linux without #ifdef proliferation.
//
// PlatformWin32.cpp: WinHTTP for HTTP, CreateFileW/CreateDirectoryW for I/O,
//                    CreateProcessW for process spawning.
// PlatformPosix.cpp: libcurl for HTTP, POSIX open/read/write/stat for I/O,
//                    fork/exec/waitpid for process spawning.

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace agent {
namespace platform {

// ---------- Platform detection ----------

enum class PlatformType {
  Windows,
  Linux,
  WSL,
  MacOS,
  Unknown,
};

PlatformType GetPlatform();
const char* PlatformTypeToString(PlatformType pt);
bool IsWindows();
bool IsLinux();

// ---------- File I/O ----------

// Read the entire contents of a file as a UTF-8 string.
// Returns empty string on failure.
std::string FileRead(const std::string& path);

// Write content to a file, creating or overwriting it.
// Returns true on success.
bool FileWrite(const std::string& path, const std::string& content);

// Check if a file exists (and is a regular file).
bool FileExists(const std::string& path);

// Check if a directory exists.
bool DirectoryExists(const std::string& path);

// Create a directory and all necessary parent directories.
bool DirectoryCreate(const std::string& path);

// Get file size in bytes. Returns -1 on failure.
int64_t FileSize(const std::string& path);

// Get the current working directory as a UTF-8 string.
std::string GetCurrentDirectory();

// Get the full absolute path for a relative path.
std::string GetFullPath(const std::string& path);

// Get the directory containing the running executable.
std::string GetExecutableDirectory();

// Mark a directory as hidden (Windows: FILE_ATTRIBUTE_HIDDEN; Linux: no-op).
void TryMarkHidden(const std::string& path);

// List files in a directory matching a glob pattern.
// Returns full paths. Non-recursive.
std::vector<std::string> ListDirectory(const std::string& dir);

// ---------- Process spawning ----------

struct ProcessResult {
  int exitCode = -1;
  std::string stdoutOutput;
  std::string stderrOutput;  // merged with stdout on Windows
  bool timedOut = false;
  bool spawnFailed = false;
  std::string errorMessage;
};

struct ProcessOptions {
  std::string command;
  std::string workingDirectory;
  std::map<std::string, std::string> env;  // additional env vars
  std::string stdinData;
  int timeoutMs = 120000;  // 2 minutes default
  bool mergeStderr = true;  // merge stderr into stdout
};

ProcessResult SpawnProcess(const ProcessOptions& options);

// ---------- HTTP ----------

struct HttpResponse {
  int statusCode = 0;
  std::string body;
  std::map<std::string, std::string> headers;
  std::string errorMessage;
  bool success() const { return statusCode >= 200 && statusCode < 300; }
};

struct HttpRequest {
  std::string method = "POST";
  std::string url;
  std::map<std::string, std::string> headers;
  std::string body;
  int timeoutMs = 600000;  // 10 minutes default (LLM calls are slow)
};

// Synchronous HTTP request.
HttpResponse HttpSend(const HttpRequest& request);

// Streaming HTTP request with SSE callback.
// `onEvent` is called for each SSE event line (data: ... lines).
// `onDone` is called when the stream completes.
// Returns false if the connection fails before streaming starts.
struct StreamCallbacks {
  std::function<void(const std::string& eventData)> onEvent;
  std::function<void()> onDone;
  std::function<void(const std::string& error)> onError;
};

bool HttpStream(const HttpRequest& request, const StreamCallbacks& callbacks);

// ---------- Timing ----------

// High-resolution monotonic tick count in milliseconds.
uint64_t GetTickCountMs();

// Sleep for the specified number of milliseconds.
void SleepMs(int ms);

// ---------- Memory info ----------

// Get the current process's resident set size (RSS) in bytes.
// Returns 0 if unavailable.
int64_t GetProcessMemoryBytes();

// Get the current process's handle count (Windows only).
// Returns 0 on Linux.
int GetProcessHandleCount();

// ---------- Console ----------

// Check if stdout is a terminal (not redirected to a pipe/file).
bool IsStdoutTerminal();

// Check if stdin is a terminal.
bool IsStdinTerminal();

// Get console width and height. Returns {80, 24} on failure.
struct ConsoleDimensions {
  int width = 80;
  int height = 24;
};
ConsoleDimensions GetConsoleDimensions();

}  // namespace platform
}  // namespace agent
