// Platform abstraction — POSIX (Linux/macOS) implementation.
// Uses libcurl for HTTP, POSIX file APIs, fork/exec for process spawning.

#ifndef _WIN32

#include "platform/Platform.h"
#include "infra/StringUtil.h"

#include <chrono>
#include <cstring>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

// libcurl for HTTP
#include <curl/curl.h>

namespace agent {
namespace platform {

// =====================================================================
// Platform detection
// =====================================================================

PlatformType GetPlatform() {
#ifdef __APPLE__
  return PlatformType::MacOS;
#else
  // Check for WSL
  struct stat st;
  if (stat("/proc/version", &st) == 0) {
    FILE* f = fopen("/proc/version", "r");
    if (f) {
      char buf[512] = {0};
      if (fgets(buf, sizeof(buf), f) && strstr(buf, "Microsoft")) {
        fclose(f);
        return PlatformType::WSL;
      }
      fclose(f);
    }
  }
  return PlatformType::Linux;
#endif
}

const char* PlatformTypeToString(PlatformType pt) {
  switch (pt) {
    case PlatformType::Windows: return "Windows";
    case PlatformType::Linux:   return "Linux";
    case PlatformType::WSL:     return "WSL";
    case PlatformType::MacOS:   return "macOS";
    default: return "Unknown";
  }
}

bool IsWindows() { return false; }
bool IsLinux() { return GetPlatform() == PlatformType::Linux ||
                         GetPlatform() == PlatformType::WSL; }

// =====================================================================
// File I/O
// =====================================================================

std::string FileRead(const std::string& path) {
  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) return {};

  struct stat st;
  if (fstat(fd, &st) < 0) { close(fd); return {}; }

  std::string content(static_cast<std::size_t>(st.st_size), '\0');
  ssize_t totalRead = 0;
  while (totalRead < st.st_size) {
    ssize_t n = read(fd, &content[totalRead],
                     static_cast<std::size_t>(st.st_size - totalRead));
    if (n <= 0) break;
    totalRead += n;
  }
  content.resize(static_cast<std::size_t>(totalRead));
  close(fd);
  return content;
}

bool FileWrite(const std::string& path, const std::string& content) {
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return false;

  ssize_t totalWritten = 0;
  while (totalWritten < static_cast<ssize_t>(content.size())) {
    ssize_t n = write(fd, content.data() + totalWritten,
                      content.size() - static_cast<std::size_t>(totalWritten));
    if (n <= 0) { close(fd); return false; }
    totalWritten += n;
  }
  fsync(fd);
  close(fd);
  return true;
}

bool FileExists(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool DirectoryExists(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool DirectoryCreate(const std::string& path) {
  if (path.empty()) return false;
  if (DirectoryExists(path)) return true;
  const std::string parent = infra::ParentPath(path);
  if (!parent.empty() && parent != path && !DirectoryExists(parent)) {
    if (!DirectoryCreate(parent)) return false;
  }
  return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

int64_t FileSize(const std::string& path) {
  struct stat st;
  if (stat(path.c_str(), &st) < 0) return -1;
  return static_cast<int64_t>(st.st_size);
}

std::string GetCurrentDirectory() {
  char buf[4096];
  if (getcwd(buf, sizeof(buf))) return std::string(buf);
  return ".";
}

std::string GetFullPath(const std::string& path) {
  char resolved[PATH_MAX];
  if (realpath(path.c_str(), resolved)) return std::string(resolved);
  return path;
}

std::string GetExecutableDirectory() {
  char buf[PATH_MAX];
  ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (len <= 0) return GetCurrentDirectory();
  buf[len] = '\0';
  return infra::ParentPath(std::string(buf));
}

void TryMarkHidden(const std::string& /*path*/) {
  // On Linux, hidden files start with '.' — no attribute to set.
  // This is a no-op; callers should use dot-prefix naming instead.
}

std::vector<std::string> ListDirectory(const std::string& dir) {
  std::vector<std::string> results;
  DIR* d = opendir(dir.c_str());
  if (!d) return results;
  struct dirent* entry;
  while ((entry = readdir(d)) != nullptr) {
    std::string name = entry->d_name;
    if (name == "." || name == "..") continue;
    results.push_back(infra::JoinPath(dir, name));
  }
  closedir(d);
  return results;
}

// =====================================================================
// Process spawning
// =====================================================================

ProcessResult SpawnProcess(const ProcessOptions& options) {
  ProcessResult result;

  int stdoutPipe[2] = {-1, -1};
  int stdinPipe[2] = {-1, -1};

  if (pipe(stdoutPipe) < 0) {
    result.spawnFailed = true;
    result.errorMessage = "pipe() failed";
    return result;
  }

  if (!options.stdinData.empty()) {
    if (pipe(stdinPipe) < 0) {
      result.spawnFailed = true;
      result.errorMessage = "pipe() stdin failed";
      close(stdoutPipe[0]); close(stdoutPipe[1]);
      return result;
    }
  }

  pid_t pid = fork();
  if (pid < 0) {
    result.spawnFailed = true;
    result.errorMessage = "fork() failed: " + std::string(strerror(errno));
    close(stdoutPipe[0]); close(stdoutPipe[1]);
    if (stdinPipe[0] >= 0) { close(stdinPipe[0]); close(stdinPipe[1]); }
    return result;
  }

  if (pid == 0) {
    // Child process
    close(stdoutPipe[0]);  // close read end
    dup2(stdoutPipe[1], STDOUT_FILENO);
    if (options.mergeStderr) dup2(stdoutPipe[1], STDERR_FILENO);
    close(stdoutPipe[1]);

    if (!options.stdinData.empty()) {
      close(stdinPipe[1]);  // close write end
      dup2(stdinPipe[0], STDIN_FILENO);
      close(stdinPipe[0]);
    }

    // Set working directory
    if (!options.workingDirectory.empty()) {
      if (chdir(options.workingDirectory.c_str()) < 0) _exit(127);
    }

    // Set environment variables
    for (const auto& kv : options.env) {
      setenv(kv.first.c_str(), kv.second.c_str(), 1);
    }

    execl("/bin/sh", "sh", "-c", options.command.c_str(), nullptr);
    _exit(127);  // exec failed
  }

  // Parent process
  close(stdoutPipe[1]);  // close write end
  if (stdinPipe[0] >= 0) {
    close(stdinPipe[0]);  // close read end
    // Write stdin data
    if (!options.stdinData.empty()) {
      write(stdinPipe[1], options.stdinData.data(), options.stdinData.size());
      close(stdinPipe[1]);
    }
  }

  // Read stdout in a separate thread to prevent deadlock
  std::string collectedStdout;
  std::thread reader([&stdoutPipe, &collectedStdout]() {
    char buffer[4096];
    ssize_t n;
    while ((n = read(stdoutPipe[0], buffer, sizeof(buffer))) > 0) {
      collectedStdout.append(buffer, static_cast<std::size_t>(n));
    }
  });

  // Wait with timeout
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(options.timeoutMs);
  bool timedOut = false;
  while (true) {
    int status = 0;
    pid_t w = waitpid(pid, &status, WNOHANG);
    if (w == pid) {
      result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
      break;
    }
    if (w < 0) {
      result.exitCode = -1;
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      kill(pid, SIGKILL);
      waitpid(pid, nullptr, 0);
      timedOut = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  reader.join();
  close(stdoutPipe[0]);

  result.stdoutOutput = std::move(collectedStdout);
  result.timedOut = timedOut;
  return result;
}

// =====================================================================
// HTTP (libcurl)
// =====================================================================

namespace {

struct CurlWriteData {
  std::string* target;
};

size_t CurlWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* data = static_cast<CurlWriteData*>(userdata);
  size_t total = size * nmemb;
  data->target->append(ptr, total);
  return total;
}

size_t CurlHeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
  auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);
  size_t total = size * nitems;
  std::string line(buffer, total);
  auto colonPos = line.find(':');
  if (colonPos != std::string::npos) {
    std::string key = line.substr(0, colonPos);
    std::string value = line.substr(colonPos + 1);
    // Trim whitespace
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(0, 1);
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) value.pop_back();
    (*headers)[key] = value;
  }
  return total;
}

}  // namespace

HttpResponse HttpSend(const HttpRequest& request) {
  HttpResponse response;

  CURL* curl = curl_easy_init();
  if (!curl) {
    response.errorMessage = "curl_easy_init failed";
    return response;
  }

  curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());

  // Timeout (in seconds for curl)
  long timeoutSec = request.timeoutMs / 1000;
  if (timeoutSec < 1) timeoutSec = 1;
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec);

  // Body
  if (!request.body.empty()) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
  }

  // Headers
  struct curl_slist* headerList = nullptr;
  for (const auto& hdr : request.headers) {
    std::string line = hdr.first + ": " + hdr.second;
    headerList = curl_slist_append(headerList, line.c_str());
  }
  if (headerList) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);

  // Response
  CurlWriteData writeData{&response.body};
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &writeData);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, CurlHeaderCallback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);

  CURLcode res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    response.errorMessage = curl_easy_strerror(res);
  } else {
    long statusCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCode);
    response.statusCode = static_cast<int>(statusCode);
  }

  if (headerList) curl_slist_free_all(headerList);
  curl_easy_cleanup(curl);
  return response;
}

bool HttpStream(const HttpRequest& request, const StreamCallbacks& callbacks) {
  CURL* curl = curl_easy_init();
  if (!curl) {
    if (callbacks.onError) callbacks.onError("curl_easy_init failed");
    return false;
  }

  curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);

  long timeoutSec = request.timeoutMs / 1000;
  if (timeoutSec < 1) timeoutSec = 1;
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec);

  if (!request.body.empty()) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
  }

  struct curl_slist* headerList = nullptr;
  for (const auto& hdr : request.headers) {
    std::string line = hdr.first + ": " + hdr.second;
    headerList = curl_slist_append(headerList, line.c_str());
  }
  if (headerList) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);

  // Use write callback for SSE streaming — process line by line
  std::string lineBuffer;
  auto writeCb = [](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
    auto* ctx = static_cast<std::pair<std::string*, StreamCallbacks*>*>(userdata);
    size_t total = size * nmemb;
    ctx->first->append(ptr, total);

    // Process complete lines
    std::size_t pos;
    while ((pos = ctx->first->find('\n')) != std::string::npos) {
      std::string line = ctx->first->substr(0, pos);
      ctx->first->erase(0, pos + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (ctx->second->onEvent) ctx->second->onEvent(line);
    }
    return total;
  };

  auto ctx = std::make_pair(&lineBuffer, const_cast<StreamCallbacks*>(&callbacks));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                   +[](char* ptr, size_t size, size_t nmemb, void* ud) -> size_t {
                     auto* c = static_cast<decltype(ctx)*>(ud);
                     size_t total = size * nmemb;
                     c->first->append(ptr, total);
                     std::size_t pos;
                     while ((pos = c->first->find('\n')) != std::string::npos) {
                       std::string line = c->first->substr(0, pos);
                       c->first->erase(0, pos + 1);
                       if (!line.empty() && line.back() == '\r') line.pop_back();
                       if (c->second->onEvent) c->second->onEvent(line);
                     }
                     return total;
                   });
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

  CURLcode res = curl_easy_perform(curl);

  // Process any remaining data in buffer
  if (!lineBuffer.empty() && callbacks.onEvent) {
    callbacks.onEvent(lineBuffer);
  }

  if (res != CURLE_OK) {
    if (callbacks.onError) callbacks.onError(curl_easy_strerror(res));
  } else {
    if (callbacks.onDone) callbacks.onDone();
  }

  if (headerList) curl_slist_free_all(headerList);
  curl_easy_cleanup(curl);
  return res == CURLE_OK;
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
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// =====================================================================
// Memory info
// =====================================================================

int64_t GetProcessMemoryBytes() {
  FILE* f = fopen("/proc/self/status", "r");
  if (!f) return 0;
  char line[256];
  int64_t rssKb = 0;
  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, "VmRSS:", 6) == 0) {
      sscanf(line + 6, " %ld", &rssKb);
      break;
    }
  }
  fclose(f);
  return rssKb * 1024;
}

int GetProcessHandleCount() {
  return 0;  // Not applicable on Linux
}

// =====================================================================
// Console
// =====================================================================

bool IsStdoutTerminal() {
  return isatty(STDOUT_FILENO) != 0;
}

bool IsStdinTerminal() {
  return isatty(STDIN_FILENO) != 0;
}

ConsoleDimensions GetConsoleDimensions() {
  ConsoleDimensions dim;
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
    dim.width = ws.ws_col;
    dim.height = ws.ws_row;
  }
  return dim;
}

}  // namespace platform
}  // namespace agent

#endif  // !_WIN32
