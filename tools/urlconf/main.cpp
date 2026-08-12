// The URL conformance runner.
//
// `src/url` is a security boundary before it is a convenience (see the header
// of url/Url.h), and the only credible check on it is the standard's own test
// vectors -- 1004 parse cases, 300-odd setter cases and 2,673 IDNA cases, all
// of them pinned in third_party/wpt. web-platform-tests exercises the same
// vectors, but through a whole browser: a page, a script, a binding layer and
// an event loop. When the parser is wrong, that path costs three minutes per
// answer and reports the failure as a subtest name.
//
// This runs the same vectors against the parser directly, in about a second,
// and prints the first N differences with the field that differed. It is a
// tool rather than a test target for the reason microbrowser_bidiconf is one:
// the data is not vendored, so it must build and run whether or not the
// checkout is there.
//
//   tools/urlconf/main.cpp <area>...   # url, setters, toascii, idna; default all
//   --show N                           # first N failures per area (default 10)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "urlconf/Json.h"
#include "url/Url.h"

namespace microbrowser::urlconf {
namespace {

using url::Url;

std::string DataRoot() {
  return std::string(MICROBROWSER_SOURCE_ROOT) + "/third_party/wpt/url/resources/";
}

JsonPtr Load(const std::string& name) {
  const std::optional<std::string> text = ReadFile(DataRoot() + name);
  if (!text.has_value()) {
    std::fprintf(stderr, "missing %s%s -- run tools/wpt/fetch.sh\n", DataRoot().c_str(),
                 name.c_str());
    return nullptr;
  }
  const JsonPtr parsed = ParseJson(*text);
  if (parsed == nullptr) {
    std::fprintf(stderr, "could not parse %s\n", name.c_str());
  }
  return parsed;
}

// Printable form of a string that may hold controls or non-ASCII, so a failure
// line is one line and the difference is visible in it.
std::string Show(std::string_view text) {
  std::string out;
  for (const char raw : text) {
    const auto c = static_cast<unsigned char>(raw);
    if (c == '\t') {
      out += "\\t";
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c < 0x20 || c == 0x7F) {
      char buffer[8];
      std::snprintf(buffer, sizeof(buffer), "\\x%02x", c);
      out += buffer;
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  return out;
}

struct Tally {
  int passed = 0;
  int failed = 0;
  int shown = 0;
  int show_limit = 10;

  void Fail(const std::string& message) {
    ++failed;
    if (shown < show_limit) {
      ++shown;
      std::printf("  %s\n", message.c_str());
    }
  }
  void Pass() { ++passed; }
  void Report(const char* area) const {
    const int total = passed + failed;
    std::printf("%s: %d/%d (%.1f%%)\n", area, passed, total,
                total == 0 ? 0.0 : 100.0 * passed / total);
  }
};

// Compares every field the vector constrains. One diagnostic per entry rather
// than per field: a wrong host makes four fields wrong and four lines of that
// is noise around one cause.
void CheckFields(const JsonValue& entry, const Url& url, const std::string& label, Tally& tally) {
  struct Field {
    const char* name;
    std::string actual;
  };
  const Field fields[] = {
      {"href", url.Href()},         {"protocol", url.Protocol()},
      {"username", url.Username()}, {"password", url.Password()},
      {"host", url.HostPort()},     {"hostname", url.Hostname()},
      {"port", url.PortString()},   {"pathname", url.Pathname()},
      {"search", url.Search()},     {"hash", url.Hash()},
      {"origin", url.OriginString()},
  };
  for (const Field& field : fields) {
    const std::optional<std::string> expected = entry.Str(field.name);
    if (!expected.has_value()) {
      continue;  // `origin` is absent from most entries, which is not "empty"
    }
    if (*expected != field.actual) {
      tally.Fail(label + ": " + field.name + " expected <" + Show(*expected) + "> got <" +
                 Show(field.actual) + ">");
      return;
    }
  }
  tally.Pass();
}

void RunUrlTests(Tally& tally) {
  const JsonPtr data = Load("urltestdata.json");
  if (data == nullptr || !data->IsArray()) {
    return;
  }
  for (const JsonPtr& element : data->array) {
    if (!element->IsObject()) {
      continue;  // a comment string
    }
    const std::optional<std::string> input = element->Str("input");
    if (!input.has_value()) {
      continue;
    }
    const JsonValue* base_value = element->Find("base");
    std::optional<Url> base;
    if (base_value != nullptr && base_value->IsString()) {
      base = Url::Parse(base_value->string);
      if (!base.has_value()) {
        tally.Fail("<" + Show(*input) + ">: base <" + Show(base_value->string) +
                   "> did not parse");
        continue;
      }
    }
    const std::optional<Url> parsed =
        base.has_value() ? Url::Parse(*input, *base) : Url::Parse(*input);

    const std::string label = "<" + Show(*input) + "> against <" +
                              (base_value != nullptr && base_value->IsString()
                                   ? Show(base_value->string)
                                   : "(none)") +
                              ">";
    if (element->Truthy("failure")) {
      if (parsed.has_value()) {
        tally.Fail(label + ": expected failure, got <" + Show(parsed->Href()) + ">");
      } else {
        tally.Pass();
      }
      continue;
    }
    if (!parsed.has_value()) {
      tally.Fail(label + ": expected success, got failure");
      continue;
    }
    CheckFields(*element, *parsed, label, tally);
  }
}

void RunSetterTests(Tally& tally) {
  const JsonPtr data = Load("setters_tests.json");
  if (data == nullptr || !data->IsObject()) {
    return;
  }
  for (const auto& [attribute, cases] : data->object) {
    if (attribute == "comment" || !cases->IsArray()) {
      continue;
    }
    for (const JsonPtr& entry : cases->array) {
      const std::optional<std::string> href = entry->Str("href");
      const std::optional<std::string> new_value = entry->Str("new_value");
      const JsonValue* expected = entry->Find("expected");
      if (!href.has_value() || !new_value.has_value() || expected == nullptr) {
        continue;
      }
      std::optional<Url> url = Url::Parse(*href);
      if (!url.has_value()) {
        tally.Fail(attribute + ": base <" + Show(*href) + "> did not parse");
        continue;
      }
      if (attribute == "protocol") {
        url->SetProtocol(*new_value);
      } else if (attribute == "username") {
        url->SetUsername(*new_value);
      } else if (attribute == "password") {
        url->SetPassword(*new_value);
      } else if (attribute == "host") {
        url->SetHost(*new_value);
      } else if (attribute == "hostname") {
        url->SetHostname(*new_value);
      } else if (attribute == "port") {
        url->SetPort(*new_value);
      } else if (attribute == "pathname") {
        url->SetPathname(*new_value);
      } else if (attribute == "search") {
        url->SetSearch(*new_value);
      } else if (attribute == "hash") {
        url->SetHash(*new_value);
      } else if (attribute == "href") {
        const std::optional<Url> replaced = Url::Parse(*new_value);
        if (replaced.has_value()) {
          url = replaced;
        }
      } else {
        continue;
      }
      CheckFields(*expected, *url, attribute + " <" + Show(*href) + "> = <" + Show(*new_value) + ">",
                  tally);
    }
  }
}

void RunToAsciiTests(Tally& tally) {
  const JsonPtr data = Load("toascii.json");
  if (data == nullptr || !data->IsArray()) {
    return;
  }
  for (const JsonPtr& entry : data->array) {
    if (!entry->IsObject()) {
      continue;
    }
    const std::optional<std::string> input = entry->Str("input");
    if (!input.has_value()) {
      continue;
    }
    const JsonValue* output = entry->Find("output");
    const std::optional<Url> parsed = Url::Parse("https://" + *input + "/x");
    const std::string label = "toascii <" + Show(*input) + ">";
    if (output == nullptr || output->IsNull()) {
      if (parsed.has_value()) {
        tally.Fail(label + ": expected failure, got <" + Show(parsed->Hostname()) + ">");
      } else {
        tally.Pass();
      }
      continue;
    }
    if (!parsed.has_value()) {
      tally.Fail(label + ": expected <" + Show(output->string) + ">, got failure");
      continue;
    }
    if (parsed->Hostname() != output->string) {
      tally.Fail(label + ": expected <" + Show(output->string) + ">, got <" +
                 Show(parsed->Hostname()) + ">");
      continue;
    }
    tally.Pass();
  }
}

// IdnaTestV2's own format: a `comment` string per section, then arrays of
// [input, ToASCII output, status, ...]. Only ToASCII is checked here, because
// that is what a URL host goes through; the ToUnicode half has no consumer in
// this browser and a test for something nothing calls is a test that drifts.
void RunIdnaTests(Tally& tally, const char* file) {
  const JsonPtr data = Load(file);
  if (data == nullptr || !data->IsArray()) {
    return;
  }
  for (const JsonPtr& entry : data->array) {
    if (!entry->IsObject()) {
      continue;
    }
    const std::optional<std::string> input = entry->Str("input");
    const JsonValue* output = entry->Find("output");
    if (!input.has_value() || output == nullptr || input->empty()) {
      continue;  // the empty domain cannot be reached through a URL, so WPT skips it too
    }
    const std::string label = std::string(file) + " <" + Show(*input) + ">";
    const std::optional<Url> parsed = Url::Parse("https://" + *input + "/x");
    if (output->IsNull()) {
      if (parsed.has_value()) {
        tally.Fail(label + ": expected failure, got <" + Show(parsed->Hostname()) + ">");
      } else {
        tally.Pass();
      }
      continue;
    }
    if (!parsed.has_value()) {
      tally.Fail(label + ": expected <" + Show(output->string) + ">, got failure");
      continue;
    }
    if (parsed->Hostname() != output->string) {
      tally.Fail(label + ": expected <" + Show(output->string) + ">, got <" +
                 Show(parsed->Hostname()) + ">");
      continue;
    }
    tally.Pass();
  }
}

}  // namespace
}  // namespace microbrowser::urlconf

int main(int argc, char** argv) {
  using namespace microbrowser::urlconf;

  int show_limit = 10;
  std::vector<std::string> areas;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--show") == 0 && i + 1 < argc) {
      show_limit = std::atoi(argv[++i]);
    } else {
      areas.emplace_back(argv[i]);
    }
  }
  if (areas.empty()) {
    areas = {"url", "setters", "toascii", "idna"};
  }

  int total_failed = 0;
  for (const std::string& area : areas) {
    Tally tally;
    tally.show_limit = show_limit;
    if (area == "url") {
      RunUrlTests(tally);
    } else if (area == "setters") {
      RunSetterTests(tally);
    } else if (area == "toascii") {
      RunToAsciiTests(tally);
    } else if (area == "idna") {
      RunIdnaTests(tally, "IdnaTestV2.json");
    } else if (area == "idna-removed") {
      RunIdnaTests(tally, "IdnaTestV2-removed.json");
    } else {
      std::fprintf(stderr, "unknown area: %s\n", area.c_str());
      return 2;
    }
    tally.Report(area.c_str());
    total_failed += tally.failed;
  }
  return total_failed == 0 ? 0 : 1;
}
