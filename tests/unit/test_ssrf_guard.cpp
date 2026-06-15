// STRENGTHEN-T17: SSRF guard unit tests.
#include "hooks/SsrfGuard.h"

#include <iostream>

static int failures = 0;

static void Check(bool condition, const char* label) {
  if (!condition) {
    std::cerr << "FAIL: " << label << std::endl;
    ++failures;
  }
}

int main() {
  using namespace agent::hooks;

  // Safe URLs pass
  Check(IsUrlSafe("https://example.com/path"), "https example allowed");
  Check(IsUrlSafe("http://example.com:8080/x"), "http with port allowed");
  Check(IsUrlSafe("https://api.anthropic.com/v1"), "api endpoint allowed");
  Check(IsUrlSafe("https://www.bing.com/search?q=test"), "bing search allowed");

  // Non-http schemes blocked
  Check(!IsUrlSafe("file:///etc/passwd"), "file:// blocked");
  Check(!IsUrlSafe("gopher://x/"), "gopher blocked");
  Check(!IsUrlSafe("ftp://example.com/"), "ftp blocked");

  // Loopback blocked
  Check(!IsUrlSafe("http://127.0.0.1/"), "127.0.0.1 blocked");
  Check(!IsUrlSafe("http://127.0.0.1:8080/"), "127.0.0.1:8080 blocked");
  Check(!IsUrlSafe("http://localhost/"), "localhost blocked");
  Check(!IsUrlSafe("http://localhost:3000/"), "localhost:3000 blocked");

  // Private ranges blocked
  Check(!IsUrlSafe("http://10.0.0.1/"), "10.x blocked");
  Check(!IsUrlSafe("http://192.168.1.1/"), "192.168.x blocked");
  Check(!IsUrlSafe("http://172.16.0.1/"), "172.16.x blocked");

  // Cloud metadata blocked (the critical SSRF target)
  Check(!IsUrlSafe("http://169.254.169.254/latest/meta-data/"),
        "AWS metadata blocked");
  Check(!IsUrlSafe("http://metadata.google.internal/"),
        "GCP metadata blocked");

  // Link-local blocked
  Check(!IsUrlSafe("http://169.254.1.1/"), "link-local blocked");

  // 0.0.0.0 blocked
  Check(!IsUrlSafe("http://0.0.0.0/"), "0.0.0.0 blocked");

  // Verify the reason string is populated for a blocked URL
  auto r = CheckUrlForSsrf("http://169.254.169.254/x");
  Check(r.verdict == SsrfVerdict::Block, "metadata verdict is Block");
  Check(!r.reason.empty(), "block reason is non-empty");

  if (failures == 0) {
    std::cout << "All SSRF guard tests PASSED" << std::endl;
    return 0;
  }
  std::cerr << "[test_ssrf_guard] Failures: " << failures << std::endl;
  return 1;
}
