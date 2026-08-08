#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "engine/DecoderClient.h"
#include "ipc/DecoderMessage.h"

namespace microbrowser::tests {

namespace {

std::string DecoderBinaryPath() {
  if (const char* env = std::getenv("MICROBROWSER_DECODER")) {
    return env;
  }
  const std::filesystem::path build =
      std::filesystem::path("build") / "microbrowser" / "microbrowser_decoder";
  if (std::filesystem::exists(build)) {
    return build.string();
  }
  return engine::DecoderClient::FindDecoderBinary();
}

}  // namespace

void RegisterDecoderClientTests(std::vector<TestCase>& tests) {
  AddTest(tests, "DecoderClient/ConfigureFlushRoundTrip", [] {
    const std::string binary = DecoderBinaryPath();
    if (!std::filesystem::exists(binary)) {
      std::fprintf(stderr, "  (skipped: microbrowser_decoder missing at %s)\n", binary.c_str());
      return;
    }
    engine::DecoderClient client;
    Expect(client.Configure(ipc::WireCodec::Opus, {}), "configure");
    Expect(client.Flush(), "flush");
    std::string error;
    (void)client.PollFrames(&error);
    // An empty Opus configure may error rather than frame; either proves the pipe works.
    Expect(error.empty() || error == "codec" || error == "not_configured", "error reason");
  });
}

}  // namespace microbrowser::tests
