// STRENGTHEN-T17: SSRF guard implementation.
#include "hooks/SsrfGuard.h"

#include "infra/EnvUtil.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <sstream>

namespace agent {
namespace hooks {

namespace {

// Extract the scheme (lowercased) from a URL. Returns empty if no scheme.
std::string ExtractScheme(const std::string& url) {
  auto colon = url.find(':');
  if (colon == std::string::npos) return {};
  std::string scheme = url.substr(0, colon);
  std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return scheme;
}

// Extract the host (lowercased) from a URL. Strips userinfo, port, path.
std::string ExtractHost(const std::string& url) {
  // Find scheme://
  auto schemeEnd = url.find("://");
  std::size_t hostStart;
  if (schemeEnd != std::string::npos) {
    hostStart = schemeEnd + 3;
  } else {
    // No scheme — treat the whole thing as host:path
    hostStart = 0;
  }
  // Strip userinfo (user:pass@)
  auto at = url.find('@', hostStart);
  if (at != std::string::npos) hostStart = at + 1;
  // Find end of host: port (:) or path (/) or end
  std::size_t hostEnd = url.size();
  for (std::size_t i = hostStart; i < url.size(); ++i) {
    if (url[i] == ':' || url[i] == '/' || url[i] == '?' || url[i] == '#') {
      hostEnd = i;
      break;
    }
  }
  std::string host = url.substr(hostStart, hostEnd - hostStart);
  std::transform(host.begin(), host.end(), host.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return host;
}

// Check if a host string is an IPv4 address. Also detect IPv6 (bracketed).
bool IsIpv4(const std::string& host) {
  int dots = 0;
  for (char c : host) {
    if (c == '.') ++dots;
    else if (c < '0' || c > '9') return false;
  }
  return dots == 3;
}

// Parse an IPv4 octet string to a uint32_t. Returns false on parse error.
bool ParseIpv4(const std::string& host, unsigned int& out) {
  unsigned int b[4] = {0, 0, 0, 0};
  int idx = 0;
  std::string cur;
  for (char c : host + ".") {
    if (c == '.') {
      if (idx > 3) return false;
      if (cur.empty() || cur.size() > 3) return false;
      int val = 0;
      for (char d : cur) { val = val * 10 + (d - '0'); }
      if (val > 255) return false;
      b[idx++] = static_cast<unsigned int>(val);
      cur.clear();
    } else if (c >= '0' && c <= '9') {
      cur += c;
    } else {
      return false;
    }
  }
  if (idx != 4) return false;
  out = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
  return true;
}

bool IsPrivateIpv4(unsigned int ip) {
  // 10.0.0.0/8
  if ((ip & 0xFF000000) == 0x0A000000) return true;
  // 172.16.0.0/12
  if ((ip & 0xFFF00000) == 0xAC100000) return true;
  // 192.168.0.0/16
  if ((ip & 0xFFFF0000) == 0xC0A80000) return true;
  // 127.0.0.0/8 (loopback)
  if ((ip & 0xFF000000) == 0x7F000000) return true;
  // 169.254.0.0/16 (link-local + cloud metadata)
  if ((ip & 0xFFFF0000) == 0xA9FE0000) return true;
  // 0.0.0.0/8
  if ((ip & 0xFF000000) == 0x00000000) return true;
  return false;
}

// Operator allowlist of domains (env: AGENT_SSRF_ALLOW_DOMAINS).
std::set<std::string> LoadAllowlist() {
  std::set<std::string> result;
  std::string raw = infra::GetEnvString("AGENT_SSRF_ALLOW_DOMAINS");
  if (raw.empty()) return result;
  std::istringstream ss(raw);
  std::string token;
  while (std::getline(ss, token, ',')) {
    // trim whitespace
    std::string trimmed;
    for (char c : token) {
      if (c != ' ' && c != '\t') trimmed += static_cast<char>(std::tolower(c));
    }
    if (!trimmed.empty()) result.insert(trimmed);
  }
  return result;
}

}  // namespace

SsrfCheckResult CheckUrlForSsrf(const std::string& url) {
  SsrfCheckResult result;

  // 1. Scheme check — only http/https allowed
  const std::string scheme = ExtractScheme(url);
  if (scheme != "http" && scheme != "https") {
    result.verdict = SsrfVerdict::Block;
    result.reason = "non-http(s) scheme blocked: '" + scheme + "'";
    return result;
  }

  const std::string host = ExtractHost(url);
  if (host.empty()) {
    result.verdict = SsrfVerdict::Block;
    result.reason = "empty host in URL";
    return result;
  }

  // 2. Operator allowlist bypass (for whitelisted internal registries)
  static const std::set<std::string> allowlist = LoadAllowlist();
  if (allowlist.count(host)) {
    result.verdict = SsrfVerdict::Allow;
    return result;
  }

  // 3. Known metadata-service hostnames
  if (host == "metadata.google.internal" ||
      host == "metadata" ||
      host == "169.254.169.254" ||
      host == "fd00:ec2::254") {
    result.verdict = SsrfVerdict::Block;
    result.reason = "cloud metadata service address blocked: " + host;
    return result;
  }

  // 4. localhost / loopback hostnames
  if (host == "localhost" || host == "ip6-localhost" ||
      host == "::1" || host == "[::1]") {
    result.verdict = SsrfVerdict::Block;
    result.reason = "loopback host blocked: " + host;
    return result;
  }

  // 5. IPv4 private-range check
  if (IsIpv4(host)) {
    unsigned int ip = 0;
    if (ParseIpv4(host, ip) && IsPrivateIpv4(ip)) {
      result.verdict = SsrfVerdict::Block;
      result.reason = "private/internal IP range blocked: " + host;
      return result;
    }
  }

  // 6. IPv6 link-local / unique-local check (fc00::/7, fe80::/10, ::1)
  if (host.size() >= 2 && host[0] == '[') {
    std::string v6 = host.substr(1);
    if (v6.size() >= 2) {
      char p1 = static_cast<char>(std::tolower(v6[0]));
      char p2 = static_cast<char>(std::tolower(v6[1]));
      // fc00::/7 covers fc.. and fd.. ; fe80::/10 covers fe80-febf
      if ((p1 == 'f' && (p2 == 'c' || p2 == 'd')) ||
          (p1 == 'f' && p2 == 'e')) {
        result.verdict = SsrfVerdict::Block;
        result.reason = "private IPv6 range blocked: " + host;
        return result;
      }
    }
  }

  result.verdict = SsrfVerdict::Allow;
  return result;
}

bool IsUrlSafe(const std::string& url) {
  return CheckUrlForSsrf(url).verdict == SsrfVerdict::Allow;
}

}  // namespace hooks
}  // namespace agent
