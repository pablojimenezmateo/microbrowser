#include "wpt/TestList.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <unordered_set>

#include "wpt/Server.h"

namespace microbrowser::wpt {
namespace {

// A test's classification only needs the head of the file: the harness script
// tag and any `rel=match` link are both in it. Reading 160,000 whole files to
// find them would make the walk cost more than the run.
constexpr std::size_t kHeadBytes = 16 * 1024;

bool EndsWith(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Directories that hold what tests *use* rather than tests. `support` and
// `resources` are WPT conventions; `reference` holds reftest references, which
// are reached by name from the test rather than run on their own.
bool IsNonTestDirectory(std::string_view name) {
  return name == "resources" || name == "support" || name == "reference" || name == "tools" ||
         name == "META.yml" || (!name.empty() && name.front() == '.');
}

bool IsNonTestFile(std::string_view name) {
  static constexpr std::string_view kSuffixes[] = {
      "-ref.html",  "-ref.htm",   "-ref.xht",  "-ref.xhtml", "-ref.svg",
      "-notref.html", "-notref.xht", "-manual.html", "-manual.htm", "-manual.https.html",
      ".headers",   ".ini",       ".py",       ".json",      ".md",
      ".txt",       ".yml",       ".css",      ".png",       ".jpg",
      ".gif",       ".webp",      ".woff",     ".woff2",     ".ttf",
      ".otf",       ".mp4",       ".webm",     ".mp3",       ".wav",
      ".ogg",       ".vtt",       ".pdf",      ".wasm",      ".bmp",
      ".ico",       ".m4a",       ".m3u8",     ".sub.txt",
  };
  for (const std::string_view suffix : kSuffixes) {
    if (EndsWith(name, suffix)) {
      return true;
    }
  }
  // `foo.helper.html` and `foo-frame.html` are loaded *by* a test.
  return name.find(".helper.") != std::string_view::npos;
}

std::string ReadHead(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return {};
  }
  std::string head(kHeadBytes, '\0');
  stream.read(head.data(), static_cast<std::streamsize>(head.size()));
  head.resize(static_cast<std::size_t>(stream.gcount()));
  return head;
}

// `<link rel="match" href="foo-ref.html">`, in any of the spellings WPT uses.
bool FindReference(std::string_view head, std::string* reference, bool* mismatch) {
  std::size_t position = 0;
  while (true) {
    const std::size_t rel = head.find("rel=", position);
    if (rel == std::string_view::npos) {
      return false;
    }
    std::string_view rest = head.substr(rel + 4);
    if (!rest.empty() && (rest.front() == '"' || rest.front() == '\'')) {
      rest.remove_prefix(1);
    }
    const bool is_match = rest.compare(0, 5, "match") == 0;
    const bool is_mismatch = rest.compare(0, 8, "mismatch") == 0;
    if (!is_match && !is_mismatch) {
      position = rel + 4;
      continue;
    }
    // The href may come before or after rel; search the enclosing tag.
    const std::size_t tag_start = head.rfind('<', rel);
    const std::size_t tag_end = head.find('>', rel);
    if (tag_start == std::string_view::npos || tag_end == std::string_view::npos) {
      return false;
    }
    const std::string_view tag = head.substr(tag_start, tag_end - tag_start);
    const std::size_t href = tag.find("href");
    if (href == std::string_view::npos) {
      position = rel + 4;
      continue;
    }
    std::size_t value = tag.find('=', href);
    if (value == std::string_view::npos) {
      position = rel + 4;
      continue;
    }
    ++value;
    while (value < tag.size() && (tag[value] == ' ' || tag[value] == '"' || tag[value] == '\'')) {
      ++value;
    }
    std::size_t end = value;
    while (end < tag.size() && tag[end] != '"' && tag[end] != '\'' && tag[end] != ' ' &&
           tag[end] != '>') {
      ++end;
    }
    *reference = std::string(tag.substr(value, end - value));
    *mismatch = is_mismatch;
    return true;
  }
}

// Resolves a reference href, which is relative to the test, against it.
std::string ResolveReference(std::string_view test_path, std::string_view href) {
  if (!href.empty() && href.front() == '/') {
    return std::string(href.substr(1));
  }
  const std::size_t slash = test_path.rfind('/');
  const std::string base =
      slash == std::string_view::npos ? "" : std::string(test_path.substr(0, slash + 1));
  std::string joined = base + std::string(href);
  // Collapse `..` the same way the server will.
  std::vector<std::string_view> segments;
  std::size_t start = 0;
  while (start <= joined.size()) {
    const std::size_t next = joined.find('/', start);
    const std::string_view segment = std::string_view(joined).substr(
        start, next == std::string::npos ? std::string::npos : next - start);
    if (segment == "..") {
      if (!segments.empty()) {
        segments.pop_back();
      }
    } else if (!segment.empty() && segment != ".") {
      segments.push_back(segment);
    }
    if (next == std::string::npos) {
      break;
    }
    start = next + 1;
  }
  std::string result;
  for (const std::string_view segment : segments) {
    if (!result.empty()) {
      result.push_back('/');
    }
    result.append(segment);
  }
  return result;
}

bool HasLongTimeout(std::string_view head) {
  const std::size_t position = head.find("name=\"timeout\"");
  const std::size_t alternate = head.find("name=timeout");
  const std::size_t at = position == std::string_view::npos ? alternate : position;
  if (at == std::string_view::npos) {
    return false;
  }
  return head.find("long", at) != std::string_view::npos &&
         head.find("long", at) - at < 40;
}

std::filesystem::path CachePath(const std::string& wpt_root) {
  return std::filesystem::path(wpt_root) / ".microbrowser-manifest.tsv";
}

std::string CheckoutRevision(const std::string& wpt_root) {
  std::ifstream head(std::filesystem::path(wpt_root) / ".git" / "HEAD");
  std::string line;
  if (head && std::getline(head, line)) {
    return line;
  }
  return "unknown";
}

bool LoadCache(const std::string& wpt_root, const std::string& revision,
               std::vector<WptTest>& out) {
  std::ifstream stream(CachePath(wpt_root));
  if (!stream) {
    return false;
  }
  std::string line;
  if (!std::getline(stream, line) || line != "#revision\t" + revision) {
    return false;
  }
  while (std::getline(stream, line)) {
    if (line.empty()) {
      continue;
    }
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true) {
      const std::size_t tab = line.find('\t', start);
      fields.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
      if (tab == std::string::npos) {
        break;
      }
      start = tab + 1;
    }
    if (fields.size() < 5) {
      continue;
    }
    WptTest test;
    test.kind = fields[0] == "R" ? TestKind::Reftest : TestKind::Testharness;
    test.url_path = fields[1];
    test.reference = fields[2];
    test.reference_mismatch = fields[3] == "1";
    test.long_timeout = fields[4] == "1";
    out.push_back(std::move(test));
  }
  return !out.empty();
}

void SaveCache(const std::string& wpt_root, const std::string& revision,
               const std::vector<WptTest>& tests) {
  std::ofstream stream(CachePath(wpt_root), std::ios::trunc);
  if (!stream) {
    return;
  }
  stream << "#revision\t" << revision << "\n";
  for (const WptTest& test : tests) {
    stream << (test.kind == TestKind::Reftest ? "R" : "T") << '\t' << test.url_path << '\t'
           << test.reference << '\t' << (test.reference_mismatch ? '1' : '0') << '\t'
           << (test.long_timeout ? '1' : '0') << '\n';
  }
}

void AppendVariants(const WptTest& base, const std::vector<std::string>& variants,
                    std::vector<WptTest>& out) {
  if (variants.empty()) {
    out.push_back(base);
    return;
  }
  for (const std::string& variant : variants) {
    WptTest copy = base;
    copy.url_path += variant;  // variants carry their own leading '?' or '#'
    out.push_back(std::move(copy));
  }
}

}  // namespace

std::vector<WptTest> EnumerateTests(const std::string& wpt_root,
                                    const std::vector<std::string>& prefixes,
                                    bool refresh_cache, std::string* error) {
  std::vector<WptTest> all;
  const std::string revision = CheckoutRevision(wpt_root);
  if (!std::filesystem::exists(wpt_root)) {
    if (error != nullptr) {
      *error = wpt_root + " does not exist; run tools/wpt/fetch.sh";
    }
    return {};
  }
  if (refresh_cache || !LoadCache(wpt_root, revision, all)) {
    all.clear();
    std::error_code code;
    std::filesystem::recursive_directory_iterator walk(
        wpt_root, std::filesystem::directory_options::skip_permission_denied, code);
    const std::filesystem::recursive_directory_iterator end;
    for (; walk != end; walk.increment(code)) {
      if (code) {
        continue;
      }
      const std::filesystem::path& path = walk->path();
      const std::string name = path.filename().string();
      if (walk->is_directory(code)) {
        if (IsNonTestDirectory(name)) {
          walk.disable_recursion_pending();
        }
        continue;
      }
      if (!walk->is_regular_file(code) || IsNonTestFile(name)) {
        continue;
      }
      const std::string relative =
          std::filesystem::relative(path, wpt_root, code).generic_string();
      if (relative.empty() || relative[0] == '.') {
        continue;
      }

      // Generated tests: the `.js` is the source, the `.html` is the test.
      if (EndsWith(name, ".any.js") || EndsWith(name, ".window.js") ||
          EndsWith(name, ".worker.js")) {
        std::ifstream stream(path, std::ios::binary);
        std::string head(kHeadBytes, '\0');
        stream.read(head.data(), static_cast<std::streamsize>(head.size()));
        head.resize(static_cast<std::size_t>(stream.gcount()));
        const MetaDirectives meta = ParseMeta(head);
        const std::string stem = relative.substr(0, relative.size() - 3);  // drop ".js"
        WptTest test;
        test.long_timeout = meta.long_timeout;
        if (EndsWith(name, ".any.js")) {
          const bool has_window = std::find(meta.globals.begin(), meta.globals.end(), "window") !=
                                  meta.globals.end();
          const bool has_worker =
              std::find(meta.globals.begin(), meta.globals.end(), "dedicatedworker") !=
              meta.globals.end();
          if (has_window) {
            test.url_path = stem + ".html";
            AppendVariants(test, meta.variants, all);
          }
          if (has_worker) {
            test.url_path = stem + ".worker.html";
            AppendVariants(test, meta.variants, all);
          }
        } else if (EndsWith(name, ".window.js")) {
          test.url_path = stem + ".html";
          AppendVariants(test, meta.variants, all);
        } else {
          test.url_path = stem + ".html";
          AppendVariants(test, meta.variants, all);
        }
        continue;
      }

      const bool is_document = EndsWith(name, ".html") || EndsWith(name, ".htm") ||
                               EndsWith(name, ".xhtml") || EndsWith(name, ".xht") ||
                               EndsWith(name, ".svg");
      if (!is_document) {
        continue;
      }
      const std::string head = ReadHead(path);
      if (head.empty()) {
        continue;
      }
      WptTest test;
      test.url_path = relative;
      test.long_timeout = HasLongTimeout(head);
      if (head.find("/resources/testharness.js") != std::string::npos) {
        test.kind = TestKind::Testharness;
        all.push_back(std::move(test));
        continue;
      }
      std::string reference;
      bool mismatch = false;
      if (FindReference(head, &reference, &mismatch) && !reference.empty()) {
        test.kind = TestKind::Reftest;
        test.reference = ResolveReference(relative, reference);
        test.reference_mismatch = mismatch;
        all.push_back(std::move(test));
      }
    }
    std::sort(all.begin(), all.end(),
              [](const WptTest& left, const WptTest& right) { return left.url_path < right.url_path; });
    SaveCache(wpt_root, revision, all);
  }

  if (prefixes.empty()) {
    return all;
  }
  std::vector<WptTest> filtered;
  for (WptTest& test : all) {
    for (const std::string& prefix : prefixes) {
      if (test.url_path.compare(0, prefix.size(), prefix) == 0) {
        filtered.push_back(std::move(test));
        break;
      }
    }
  }
  return filtered;
}

}  // namespace microbrowser::wpt
