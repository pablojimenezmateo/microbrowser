#include "wpt/Expectations.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>

namespace microbrowser::wpt {

std::string NormalizePortsInName(std::string_view name,
                                 const std::vector<std::uint16_t>& ports) {
  // The overwhelming majority of names have no digits after a colon at all, so the
  // cheap rejection comes first: this runs once per subtest and there are tens of
  // thousands of them per area.
  if (name.find(':') == std::string_view::npos) {
    return std::string(name);
  }
  std::string result(name);
  for (std::size_t index = 0; index < ports.size(); ++index) {
    if (ports[index] == 0) {
      continue;  // never bound; substituting "0" would match a name that says `:0`
    }
    const std::string needle = ":" + std::to_string(ports[index]);
    const std::string replacement = ":{{port[" + std::to_string(index) + "]}}";
    for (std::size_t at = result.find(needle); at != std::string::npos;
         at = result.find(needle, at + replacement.size())) {
      // Only when the digits *end* there. Without this, port 8000 would rewrite the
      // `:80001` in a name that happens to contain one, and the result would be a
      // key no run reproduces.
      const std::size_t after = at + needle.size();
      if (after < result.size() && result[after] >= '0' && result[after] <= '9') {
        at = after;
        continue;
      }
      result.replace(at, needle.size(), replacement);
    }
  }
  return result;
}

namespace {

// The top-level WPT directory a test belongs to, which is the file its
// expectation lives in.
std::string TopLevel(const std::string& url_path) {
  const std::size_t slash = url_path.find('/');
  return slash == std::string::npos ? url_path : url_path.substr(0, slash);
}

// A subtest's name is a string the *page* chose, and this file is line-based.
// A name can therefore contain a newline, or -- the case that actually bit --
// end in a space, which is what `test(function(){...})` with no name gets when
// the page's `<title>` is written with spaces inside the tags. Neither survives
// a line-based file that trims: what the runner then prints is
// `FAIL (expected PASS)` beside `MISSING (expected FAIL)` for the same subtest,
// which reads exactly like a regression and is not one. Twelve of `dom/`'s
// subtests were in that state, deterministically, against the very binary that
// recorded them.
//
// Only a name that needs it is escaped, and one that does not is written
// exactly as before. That is not cosmetic: testharness itself escapes control
// characters into names -- `Blob with type "\timage/gif\t"` is a *literal*
// backslash and `t` -- so a scheme that escaped unconditionally would
// reinterpret thousands of already-recorded names and silently invalidate every
// expectation file that was not re-recorded on the same commit.
//
// The encoded form is marked on the **key**, not on the value: `FAIL=x` is a
// raw name and `FAIL:esc=x` is an escaped one. A quoted-value convention was
// tried first and is wrong here -- `FAIL="U+fffd" should match with "#\u0000"`
// is already in the corpus, raw, and any rule that reads a leading quote as
// "this is encoded" mangles it. A status never contains a colon, so the marker
// cannot collide with anything already recorded, and every line that does not
// need it stays byte-identical.
constexpr std::string_view kEscapedSuffix = ":esc";

// What a line-based file cannot carry as written: a name that ends in
// whitespace, which the loader strips, or one that spans lines.
//
// A backslash is deliberately **not** in this list. It survives the raw path
// untouched, and adding it would put `:esc` on the several thousand names
// testharness has already escaped into the corpus itself -- churn with no
// round-trip to buy it.
bool NeedsEscaping(std::string_view value) {
  if (value.empty()) {
    return false;
  }
  if (value.back() == ' ' || value.back() == '\t') {
    return true;
  }
  return value.find_first_of("\n\r") != std::string_view::npos;
}

std::string Encode(std::string_view value) {
  std::string out;
  for (const char character : value) {
    switch (character) {
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out.push_back(character); break;
    }
  }
  return out;
}

std::string Decode(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] != '\\' || i + 1 == value.size()) {
      out.push_back(value[i]);
      continue;
    }
    switch (value[++i]) {
      case 'n': out.push_back('\n'); break;
      case 'r': out.push_back('\r'); break;
      case 't': out.push_back('\t'); break;
      case '\\': out.push_back('\\'); break;
      // An escape this writer never produces. Kept as written rather than
      // dropped, so an unknown sequence cannot silently change a name.
      default: out.push_back('\\'); out.push_back(value[i]); break;
    }
  }
  return out;
}

// One line: `FAIL=name`, or `FAIL:esc=escaped-name` when the raw form would not
// survive the round trip. Both halves of that decision are made here so they
// cannot disagree -- escaping the value while writing the plain key is how the
// first attempt at this rewrote several thousand names that were already fine.
std::string Line(std::string_view status, std::string_view value) {
  if (!NeedsEscaping(value)) {
    return std::string(status) + "=" + std::string(value) + "\n";
  }
  return std::string(status) + std::string(kEscapedSuffix) + "=" + Encode(value) + "\n";
}

std::string Serialize(const std::string& url_path, const TestExpectation& expectation) {
  std::string text;
  for (const std::string& comment : expectation.comments) {
    text += comment + "\n";
  }
  text += "[" + url_path + "]\n";
  if (expectation.disabled) {
    text += Line("disabled", expectation.disabled_reason);
  }
  if (expectation.harness != "OK") {
    text += "harness=" + expectation.harness + "\n";
  }
  for (const auto& [name, status] : expectation.subtests) {
    text += Line(status, name);
  }
  return text;
}

}  // namespace

void ExpectationStore::Load(const std::string& directory) {
  std::error_code code;
  if (!std::filesystem::is_directory(directory, code)) {
    return;
  }
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(directory, code)) {
    if (!entry.is_regular_file(code) || entry.path().extension() != ".txt") {
      continue;
    }
    std::ifstream stream(entry.path());
    std::string line;
    std::string current;
    // Comments seen since the last blank or non-comment line: they belong to
    // the `[path]` that follows them.
    std::vector<std::string> pending_comments;
    while (std::getline(stream, line)) {
      // Only the line ending: a trailing space belongs to the name, and
      // trimming it is what made a name ending in one unmatchable.
      while (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (line.empty()) {
        pending_comments.clear();  // a blank line detaches a comment from what follows
        continue;
      }
      if (line[0] == '#') {
        pending_comments.push_back(line);
        continue;
      }
      if (line.front() == '[' && line.back() == ']') {
        current = line.substr(1, line.size() - 2);
        tests_[current].comments = std::move(pending_comments);
        pending_comments.clear();
        continue;
      }
      pending_comments.clear();
      if (current.empty()) {
        continue;
      }
      const std::size_t equals = line.find('=');
      if (equals == std::string::npos) {
        continue;
      }
      std::string key = line.substr(0, equals);
      std::string value = line.substr(equals + 1);
      if (key.size() > kEscapedSuffix.size() &&
          std::string_view(key).substr(key.size() - kEscapedSuffix.size()) == kEscapedSuffix) {
        key.resize(key.size() - kEscapedSuffix.size());
        value = Decode(value);
      }
      TestExpectation& expectation = tests_[current];
      if (key == "harness") {
        expectation.harness = value;
      } else if (key == "disabled") {
        expectation.disabled = true;
        expectation.disabled_reason = value;
      } else {
        // Anything else is a subtest status, and the value is the name -- which
        // is why the status is the key: a subtest name can contain '=' and a
        // status never can.
        expectation.subtests[value] = key;
      }
    }
  }
}

const TestExpectation* ExpectationStore::Find(const std::string& url_path) const {
  const auto found = tests_.find(url_path);
  return found == tests_.end() ? nullptr : &found->second;
}

void ExpectationStore::Set(const std::string& url_path, TestExpectation expectation) {
  if (expectation.harness == "OK" && expectation.subtests.empty() && !expectation.disabled) {
    // Everything passes now, so the comment goes with the entry: a note saying
    // why a test may never pass is wrong the moment it does.
    tests_.erase(url_path);
    return;
  }
  // The observed result carries statuses; the comment is the *file's*, written
  // by a person to say why a failure is deliberate. Carried across rather than
  // overwritten, which is the whole point of storing it.
  const auto existing = tests_.find(url_path);
  if (existing != tests_.end() && expectation.comments.empty()) {
    expectation.comments = existing->second.comments;
  }
  tests_[url_path] = std::move(expectation);
}

bool ExpectationStore::Save(const std::string& directory, std::string* error) const {
  std::error_code code;
  std::filesystem::create_directories(directory, code);
  std::map<std::string, std::string> files;
  for (const auto& [url_path, expectation] : tests_) {
    files[TopLevel(url_path)] += Serialize(url_path, expectation);
  }
  // A file that is now empty must be removed rather than left stale: an
  // expectation file that still exists after its last failure is fixed reads as
  // "these still fail".
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(directory, code)) {
    if (entry.is_regular_file(code) && entry.path().extension() == ".txt" &&
        files.find(entry.path().stem().string()) == files.end()) {
      std::filesystem::remove(entry.path(), code);
    }
  }
  for (const auto& [name, contents] : files) {
    const std::filesystem::path path = std::filesystem::path(directory) / (name + ".txt");
    std::ifstream existing(path);
    if (existing) {
      std::ostringstream buffer;
      buffer << existing.rdbuf();
      if (buffer.str() == contents) {
        continue;
      }
    }
    std::ofstream stream(path, std::ios::trunc);
    if (!stream) {
      if (error != nullptr) {
        *error = "could not write " + path.string();
      }
      return false;
    }
    stream << contents;
  }
  return true;
}

}  // namespace microbrowser::wpt
