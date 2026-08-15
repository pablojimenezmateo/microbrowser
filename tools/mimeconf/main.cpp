// MIME type parse/serialize against the MIME Sniffing Standard's own vectors.
//
// The table is pinned in third_party/wpt/mimesniff/mime-types/resources/.
// web-platform-tests runs the same rows through Blob, File, Request and
// Response; this runs them against src/util directly so a wrong quote or a
// skipped duplicate names the field in about a second. See tools/urlconf for
// the same arrangement on URL.

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

#include "urlconf/Json.h"
#include "util/MimeType.h"

namespace {

using microbrowser::urlconf::JsonPtr;
using microbrowser::urlconf::JsonValue;
using microbrowser::util::BlobMimeType;
using microbrowser::util::ParseMimeType;

std::string DataRoot() {
  return std::string(MICROBROWSER_SOURCE_ROOT) + "/third_party/wpt/mimesniff/mime-types/resources/";
}

JsonPtr Load(const std::string& name) {
  const std::optional<std::string> text = microbrowser::urlconf::ReadFile(DataRoot() + name);
  if (!text.has_value()) {
    return nullptr;
  }
  return microbrowser::urlconf::ParseJson(*text);
}

int RunFile(const char* name, int show) {
  const JsonPtr root = Load(name);
  if (root == nullptr || !root->IsArray()) {
    std::fprintf(stderr, "mimeconf: missing or malformed %s (is third_party/wpt checked out?)\n",
                 name);
    return 1;
  }
  int tested = 0;
  int failed = 0;
  int shown = 0;
  for (const JsonPtr& entry : root->array) {
    if (entry == nullptr || !entry->IsObject()) {
      continue;  // section headings are strings
    }
    const JsonValue* input = entry->Find("input");
    const JsonValue* output = entry->Find("output");
    if (input == nullptr || !input->IsString() || output == nullptr) {
      continue;
    }
    ++tested;
    const std::string got = BlobMimeType(input->string);
    const bool expect_fail = output->IsNull();
    const bool pass = expect_fail ? got.empty() : (output->IsString() && got == output->string);
    if (pass) {
      continue;
    }
    ++failed;
    if (shown < show) {
      ++shown;
      std::fprintf(stderr, "FAIL input=%s\n  got     %s\n  want    %s\n", input->string.c_str(),
                   got.empty() ? "(failure)" : got.c_str(),
                   expect_fail ? "(failure)" : output->string.c_str());
    }
  }
  std::printf("%s: %d tested, %d failed\n", name, tested, failed);
  return failed == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  int show = 20;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    if (argument == "--show" && i + 1 < argc) {
      show = std::atoi(argv[++i]);
    }
  }
  int status = 0;
  status |= RunFile("mime-types.json", show);
  status |= RunFile("generated-mime-types.json", show);
  return status;
}
