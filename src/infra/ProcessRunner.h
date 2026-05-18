#pragma once

#include <map>
#include <string>
#include <vector>

namespace agent {
namespace infra {

struct ProcessRunOptions {
  std::string executable;
  std::vector<std::string> arguments;
  std::string workingDirectory;
  std::string stdinData;
  std::vector<std::pair<std::string, std::string>> envVars;
  unsigned long timeoutMs = 30000;
};

struct ProcessRunResult {
  int exitCode = -1;
  bool timedOut = false;
  bool spawnFailed = false;
  std::string stdoutText;
  std::string stderrText;
  std::string errorMessage;
};

class ProcessRunner {
 public:
  ProcessRunResult Run(const ProcessRunOptions& options) const;

 private:
  std::wstring BuildCommandLine(const ProcessRunOptions& options) const;
  std::wstring ToWide(const std::string& text) const;
  std::string ToUtf8(const std::wstring& text) const;
  std::wstring QuoteWindowsArg(const std::string& value) const;
};

}  // namespace infra
}  // namespace agent
