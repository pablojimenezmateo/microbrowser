#include <cstddef>
#include <cstdint>
#include <string_view>

#include "url/Origin.h"
#include "url/PartitionKey.h"
#include "url/Url.h"

// The URL parser, fed arbitrary bytes.
//
// Every same-origin check, cookie scope and partition key in the browser is
// computed from this parser's output, so the interesting property is not only
// that it does not crash but that its output is *stable*: a URL that parses,
// serializes and reparses must produce the same URL. A parser whose output
// reparses to something else is a parser two components can disagree about,
// which is where origin-confusion bugs come from.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  const auto url = microbrowser::url::Url::Parse(input);
  if (!url.has_value()) {
    return 0;
  }

  const std::string serialized = url->Serialize();
  const auto reparsed = microbrowser::url::Url::Parse(serialized);
  if (!reparsed.has_value() || reparsed->Serialize() != serialized) {
    __builtin_trap();  // serialization is not a fixed point
  }

  microbrowser::url::Origin::FromUrl(*url);
  microbrowser::url::PartitionKey::ForTopLevel(microbrowser::url::ContainerId::Default(), *url);

  // Relative resolution against a parsed base must also stay total.
  microbrowser::url::Url::Parse("../a/b?c#d", *url);
  return 0;
}
