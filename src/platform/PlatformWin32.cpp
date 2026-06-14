// Platform abstraction — Windows implementation.
// Consolidates WinHTTP, Win32 file I/O, CreateProcessW, and console APIs
// from ModelClient.cpp, main.cpp, ProcessRunner.cpp, SessionManager.cpp,
// and ToolOrchestrator.cpp into a single translation unit.

#ifdef _WIN32

#include "platform/Platform.h"
#include "infra/StringUtil.h"

#include <windows.h>
#include <winhttp.h>
#include <io.h>
#include <psapi.h>

#include <atomic>
#include <chrono>
#include <sstream>
#include <thread>
#include <vector>

#pragma comment(lib, "psapi.lib")

namespace agent {
namespace platform {

using infra::Utf8ToWide;
using infra::WideToUtf8;

// =====================================================================
// Platform detection
// =====================================================================

PlatformType GetPlatform() { return PlatformType::Windows; }
const char* PlatformTypeToString(PlatformType pt) {
  switch (pt) {
    case PlatformType::Windows: return "Windows";
    case PlatformType::Linux:   return "Linux";
    case PlatformType::WSL:     return "WSL";
    case PlatformType::MacOS:   return "macOS";
    default: return "Unknown";
  }
}
bool IsWindows() { return true; }
bool IsLinux() { return false; }

// =====================================================================
// File I/O
// =====================================================================

std::string FileRead(const std::string& path) {
  HANDLE handle = CreateFileW(Utf8ToWide(path).c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return {};

  LARGE_INTEGER size;
  if (!GetFileSizeEx(handle, &size) || size.QuadPart < 0) {
    CloseHandle(handle);
    return {};
  }

  std::string content(static_cast<std::size_t>(size.QuadPart), '\0');
  DWORD totalRead = 0;
  while (totalRead < static_cast<DWORD>(content.size())) {
    DWORD chunkRead = 0;
    if (!ReadFile(handle, &content[totalRead],
                  static_cast<DWORD>(content.size()) - totalRead,
                  &chunkRead, nullptr)) {
      CloseHandle(handle);
      return {};
    }
    if (chunkRead == 0) break;
    totalRead += chunkRead;
  }
  content.resize(totalRead);
  CloseHandle(handle);
  return content;
}

bool FileWrite(const std::string& path, const std::string& content) {
  HANDLE handle = CreateFileW(Utf8ToWide(path).c_str(), GENERIC_WRITE, 0,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;

  DWORD totalWritten = 0;
  while (totalWritten < static_cast<DWORD>(content.size())) {
    DWORD written = 0;
    if (!WriteFile(handle, content.data() + totalWritten,
                   static_cast<DWORD>(content.size()) - totalWritten,
                   &written, nullptr)) {
      CloseHandle(handle);
      return false;
    }
    totalWritten += written;
  }
  FlushFileBuffers(handle);
  CloseHandle(handle);
  return true;
}

bool FileExists(const std::string& path) {
  DWORD attrs = GetFileAttributesW(Utf8ToWide(path).c_str());
  return attrs != INVALID_FILE_ATTRIBUTES &&
         (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool DirectoryExists(const std::string& path) {
  DWORD attrs = GetFileAttributesW(Utf8ToWide(path).c_str());
  return attrs != INVALID_FILE_ATTRIBUTES &&
         (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool DirectoryCreate(const std::string& path) {
  if (path.empty()) return false;
  if (DirectoryExists(path)) return true;
  const std::string parent = infra::ParentPath(path);
  if (!parent.empty() && parent != path && !DirectoryExists(parent)) {
    if (!DirectoryCreate(parent)) return false;
  }
  if (CreateDirectoryW(Utf8ToWide(path).c_str(), nullptr)) return true;
  return GetLastError() == ERROR_ALREADY_EXISTS;
}

int64_t FileSize(const std::string& path) {
  HANDLE handle = CreateFileW(Utf8ToWide(path).c_str(), GENERIC_READ,
                              FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return -1;
  LARGE_INTEGER size;
  if (!GetFileSizeEx(handle, &size)) {
    CloseHandle(handle);
    return -1;
  }
  CloseHandle(handle);
  return static_cast<int64_t>(size.QuadPart);
}

std::string GetCurrentDirectory() {
  std::vector<wchar_t> buffer(32768, L'\0');
  DWORD length = ::GetCurrentDirectoryW(
      static_cast<DWORD>(buffer.size()), &buffer[0]);
  if (length == 0 || length >= buffer.size()) return ".";
  return WideToUtf8(std::wstring(&buffer[0], length));
}

std::string GetFullPath(const std::string& path) {
  if (path.empty()) return {};
  std::vector<wchar_t> buffer(32768, L'\0');
  DWORD length = GetFullPathNameW(
      Utf8ToWide(path).c_str(), static_cast<DWORD>(buffer.size()),
      &buffer[0], nullptr);
  if (length == 0 || length >= buffer.size()) return {};
  return infra::NormalizeSeparators(WideToUtf8(std::wstring(&buffer[0], length)));
}

std::string GetExecutableDirectory() {
  std::vector<wchar_t> buffer(32768, L'\0');
  DWORD length = GetModuleFileNameW(nullptr, &buffer[0],
                                     static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) return GetCurrentDirectory();
  return infra::ParentPath(WideToUtf8(std::wstring(&buffer[0], length)));
}

void TryMarkHidden(const std::string& path) {
  if (path.empty()) return;
  const std::wstring widePath = Utf8ToWide(path);
  DWORD attrs = GetFileAttributesW(widePath.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) return;
  if ((attrs & FILE_ATTRIBUTE_HIDDEN) != 0) return;
  SetFileAttributesW(widePath.c_str(), attrs | FILE_ATTRIBUTE_HIDDEN);
}

std::vector<std::string> ListDirectory(const std::string& dir) {
  std::vector<std::string> results;
  std::string pattern = infra::JoinPath(dir, "*");
  WIN32_FIND_DATAW fd;
  HANDLE hFind = FindFirstFileW(Utf8ToWide(pattern).c_str(), &fd);
  if (hFind == INVALID_HANDLE_VALUE) return results;
  do {
    std::string name = WideToUtf8(fd.cFileName);
    if (name == "." || name == "..") continue;
    results.push_back(infra::JoinPath(dir, name));
  } while (FindNextFileW(hFind, &fd));
  FindClose(hFind);
  return results;
}

// =====================================================================
// Process spawning
// =====================================================================

ProcessResult SpawnProcess(const ProcessOptions& options) {
  ProcessResult result;

  SECURITY_ATTRIBUTES sa;
  sa.nLength = sizeof(SECURITY_ATTRIBUTES);
  sa.lpSecurityDescriptor = nullptr;
  sa.bInheritHandle = TRUE;

  HANDLE stdoutRead = nullptr;
  HANDLE stdoutWrite = nullptr;
  if (!CreatePipe(&stdoutRead, &stdoutWrite, &sa, 0)) {
    result.spawnFailed = true;
    result.errorMessage = "CreatePipe stdout failed";
    return result;
  }
  SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);

  HANDLE stdinRead = nullptr;
  HANDLE stdinWrite = nullptr;
  if (!options.stdinData.empty()) {
    if (!CreatePipe(&stdinRead, &stdinWrite, &sa, 0)) {
      result.spawnFailed = true;
      result.errorMessage = "CreatePipe stdin failed";
      CloseHandle(stdoutRead);
      CloseHandle(stdoutWrite);
      return result;
    }
    SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0);
  }

  STARTUPINFOW si;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = options.stdinData.empty() ? GetStdHandle(STD_INPUT_HANDLE) : stdinRead;
  si.hStdOutput = stdoutWrite;
  si.hStdError = options.mergeStderr ? stdoutWrite : GetStdHandle(STD_ERROR_HANDLE);

  PROCESS_INFORMATION pi;
  ZeroMemory(&pi, sizeof(pi));

  std::wstring cmdLine = Utf8ToWide(options.command);
  std::vector<wchar_t> mutableCmd(cmdLine.begin(), cmdLine.end());
  mutableCmd.push_back(L'\0');

  // Build environment block
  std::wstring envBlock;
  if (!options.env.empty()) {
    // Get current environment and merge
    LPWCH currentEnv = GetEnvironmentStringsW();
    if (currentEnv) {
      LPWCH ptr = currentEnv;
      while (*ptr) {
        std::wstring entry(ptr);
        envBlock += entry;
        envBlock += L'\0';
        ptr += entry.size() + 1;
      }
      FreeEnvironmentStringsW(currentEnv);
    }
    for (const auto& kv : options.env) {
      envBlock += Utf8ToWide(kv.first + "=" + kv.second);
      envBlock += L'\0';
    }
    envBlock += L'\0';
  }

  const std::wstring workDir = options.workingDirectory.empty()
                                   ? std::wstring()
                                   : Utf8ToWide(options.workingDirectory);

  DWORD createFlags = CREATE_NO_WINDOW;
  if (!envBlock.empty()) createFlags |= CREATE_UNICODE_ENVIRONMENT;

  if (!CreateProcessW(
          nullptr, &mutableCmd[0], nullptr, nullptr, TRUE,
          createFlags,
          envBlock.empty() ? nullptr : const_cast<wchar_t*>(envBlock.c_str()),
          workDir.empty() ? nullptr : workDir.c_str(), &si, &pi)) {
    result.spawnFailed = true;
    result.errorMessage = "CreateProcessW failed: " +
                          std::to_string(GetLastError());
    CloseHandle(stdoutRead);
    CloseHandle(stdoutWrite);
    if (stdinRead) CloseHandle(stdinRead);
    if (stdinWrite) CloseHandle(stdinWrite);
    return result;
  }

  CloseHandle(stdoutWrite);
  if (stdinRead) CloseHandle(stdinRead);

  // Write stdin data if provided
  if (!options.stdinData.empty() && stdinWrite) {
    DWORD written = 0;
    WriteFile(stdinWrite, options.stdinData.data(),
              static_cast<DWORD>(options.stdinData.size()), &written, nullptr);
    CloseHandle(stdinWrite);
    stdinWrite = nullptr;
  }

  // Read stdout in a separate thread to prevent pipe deadlock
  std::atomic<bool> processDone{false};
  std::string collectedStdout;
  std::thread stdoutReader([&stdoutRead, &processDone, &collectedStdout]() {
    char buffer[4096];
    DWORD bytesRead = 0;
    while (ReadFile(stdoutRead, buffer, sizeof(buffer), &bytesRead, nullptr) &&
           bytesRead > 0) {
      collectedStdout.append(buffer, bytesRead);
    }
  });

  // Wait for process with timeout
  DWORD waitResult = WaitForSingleObject(pi.hProcess, options.timeoutMs);
  if (waitResult == WAIT_TIMEOUT) {
    TerminateProcess(pi.hProcess, 1);
    result.timedOut = true;
  }

  processDone = true;
  DWORD exitCode = 0;
  GetExitCodeProcess(pi.hProcess, &exitCode);
  result.exitCode = static_cast<int>(exitCode);

  stdoutReader.join();
  result.stdoutOutput = std::move(collectedStdout);

  CloseHandle(stdoutRead);
  if (stdinWrite) CloseHandle(stdinWrite);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);

  return result;
}

// =====================================================================
// HTTP (WinHTTP)
// =====================================================================

HttpResponse HttpSend(const HttpRequest& request) {
  HttpResponse response;

  URL_COMPONENTS components;
  ZeroMemory(&components, sizeof(components));
  components.dwStructSize = sizeof(components);
  wchar_t hostName[256] = {0};
  wchar_t urlPath[2048] = {0};
  std::wstring wideUrl = Utf8ToWide(request.url);
  components.lpszHostName = hostName;
  components.dwHostNameLength = 256;
  components.lpszUrlPath = urlPath;
  components.dwUrlPathLength = 2048;
  if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &components)) {
    response.errorMessage = "WinHttpCrackUrl failed";
    return response;
  }

  const bool secure = components.nScheme == INTERNET_SCHEME_HTTPS;
  const std::wstring host(components.lpszHostName, components.dwHostNameLength);
  const std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);

  HINTERNET session = WinHttpOpen(L"cpp-agent/1.0",
      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    response.errorMessage = "WinHttpOpen failed";
    return response;
  }

  int resolveTimeout = std::min(request.timeoutMs, 30000);
  int connectTimeout = std::min(request.timeoutMs, 30000);
  int sendTimeout = request.timeoutMs;
  int receiveTimeout = request.timeoutMs;
  WinHttpSetTimeouts(session, resolveTimeout, connectTimeout, sendTimeout, receiveTimeout);

  HINTERNET connect = WinHttpConnect(session, host.c_str(), components.nPort, 0);
  if (!connect) {
    response.errorMessage = "WinHttpConnect failed";
    WinHttpCloseHandle(session);
    return response;
  }

  HINTERNET req = WinHttpOpenRequest(
      connect, Utf8ToWide(request.method).c_str(), path.c_str(),
      nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
      secure ? WINHTTP_FLAG_SECURE : 0);
  if (!req) {
    response.errorMessage = "WinHttpOpenRequest failed";
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return response;
  }

  // Add headers
  for (const auto& hdr : request.headers) {
    std::wstring headerLine = Utf8ToWide(hdr.first + ": " + hdr.second + "\r\n");
    WinHttpAddRequestHeaders(req, headerLine.c_str(),
                             static_cast<DWORD>(headerLine.size()),
                             WINHTTP_ADDREQ_FLAG_ADD);
  }

  // Send request
  if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                          request.body.empty() ? nullptr
                                               : const_cast<char*>(request.body.data()),
                          static_cast<DWORD>(request.body.size()),
                          static_cast<DWORD>(request.body.size()), 0)) {
    response.errorMessage = "WinHttpSendRequest failed";
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return response;
  }

  if (!WinHttpReceiveResponse(req, nullptr)) {
    response.errorMessage = "WinHttpReceiveResponse failed";
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return response;
  }

  DWORD statusCode = 0;
  DWORD statusCodeSize = sizeof(statusCode);
  WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize,
                      WINHTTP_NO_HEADER_INDEX);
  response.statusCode = static_cast<int>(statusCode);

  // Read response body
  std::string body;
  DWORD bytesRead = 0;
  char buffer[16384];
  while (WinHttpReadData(req, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
    body.append(buffer, bytesRead);
    bytesRead = 0;
  }
  response.body = std::move(body);

  WinHttpCloseHandle(req);
  WinHttpCloseHandle(connect);
  WinHttpCloseHandle(session);
  return response;
}

bool HttpStream(const HttpRequest& request, const StreamCallbacks& callbacks) {
  // For streaming, we use the same WinHTTP setup but read incrementally
  URL_COMPONENTS components;
  ZeroMemory(&components, sizeof(components));
  components.dwStructSize = sizeof(components);
  wchar_t hostName[256] = {0};
  wchar_t urlPath[2048] = {0};
  std::wstring wideUrl = Utf8ToWide(request.url);
  components.lpszHostName = hostName;
  components.dwHostNameLength = 256;
  components.lpszUrlPath = urlPath;
  components.dwUrlPathLength = 2048;
  if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &components)) {
    if (callbacks.onError) callbacks.onError("WinHttpCrackUrl failed");
    return false;
  }

  const bool secure = components.nScheme == INTERNET_SCHEME_HTTPS;
  const std::wstring host(components.lpszHostName, components.dwHostNameLength);
  const std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);

  HINTERNET session = WinHttpOpen(L"cpp-agent-stream/1.0",
      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    if (callbacks.onError) callbacks.onError("WinHttpOpen failed");
    return false;
  }

  WinHttpSetTimeouts(session, 30000, 30000, request.timeoutMs, request.timeoutMs);

  HINTERNET connect = WinHttpConnect(session, host.c_str(), components.nPort, 0);
  if (!connect) {
    if (callbacks.onError) callbacks.onError("WinHttpConnect failed");
    WinHttpCloseHandle(session);
    return false;
  }

  HINTERNET req = WinHttpOpenRequest(
      connect, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
  if (!req) {
    if (callbacks.onError) callbacks.onError("WinHttpOpenRequest failed");
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return false;
  }

  for (const auto& hdr : request.headers) {
    std::wstring headerLine = Utf8ToWide(hdr.first + ": " + hdr.second + "\r\n");
    WinHttpAddRequestHeaders(req, headerLine.c_str(),
                             static_cast<DWORD>(headerLine.size()),
                             WINHTTP_ADDREQ_FLAG_ADD);
  }

  if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                          const_cast<char*>(request.body.data()),
                          static_cast<DWORD>(request.body.size()),
                          static_cast<DWORD>(request.body.size()), 0)) {
    if (callbacks.onError) callbacks.onError("WinHttpSendRequest failed");
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return false;
  }

  if (!WinHttpReceiveResponse(req, nullptr)) {
    if (callbacks.onError) callbacks.onError("WinHttpReceiveResponse failed");
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return false;
  }

  // Read SSE events line by line
  std::string lineBuffer;
  DWORD bytesAvailable = 0;
  char buffer[8192];

  while (true) {
    if (!WinHttpQueryDataAvailable(req, &bytesAvailable) || bytesAvailable == 0) {
      // Check if there's a remaining line
      if (!lineBuffer.empty() && callbacks.onEvent) {
        callbacks.onEvent(lineBuffer);
        lineBuffer.clear();
      }
      break;
    }

    DWORD toRead = std::min(bytesAvailable, static_cast<DWORD>(sizeof(buffer)));
    DWORD bytesRead = 0;
    if (!WinHttpReadData(req, buffer, toRead, &bytesRead) || bytesRead == 0) break;

    lineBuffer.append(buffer, bytesRead);

    // Process complete lines
    std::size_t pos;
    while ((pos = lineBuffer.find('\n')) != std::string::npos) {
      std::string line = lineBuffer.substr(0, pos);
      lineBuffer.erase(0, pos + 1);
      // Strip \r
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (callbacks.onEvent) callbacks.onEvent(line);
    }
  }

  if (callbacks.onDone) callbacks.onDone();

  WinHttpCloseHandle(req);
  WinHttpCloseHandle(connect);
  WinHttpCloseHandle(session);
  return true;
}

// =====================================================================
// Timing
// =====================================================================

uint64_t GetTickCountMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
}

void SleepMs(int ms) {
  ::Sleep(static_cast<DWORD>(ms));
}

// =====================================================================
// Memory info
// =====================================================================

int64_t GetProcessMemoryBytes() {
  PROCESS_MEMORY_COUNTERS pmc;
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
    return static_cast<int64_t>(pmc.WorkingSetSize);
  }
  return 0;
}

int GetProcessHandleCount() {
  DWORD count = 0;
  ::GetProcessHandleCount(GetCurrentProcess(), &count);
  return static_cast<int>(count);
}

// =====================================================================
// Console
// =====================================================================

bool IsStdoutTerminal() {
  return _isatty(_fileno(stdout)) != 0;
}

bool IsStdinTerminal() {
  return _isatty(_fileno(stdin)) != 0;
}

ConsoleDimensions GetConsoleDimensions() {
  ConsoleDimensions dim;
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
    dim.width = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    dim.height = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
  }
  return dim;
}

}  // namespace platform
}  // namespace agent

#endif  // _WIN32
