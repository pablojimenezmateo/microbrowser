// Runs one JavaScript file and prints what it said and what it threw.
//
// The same argument the snapshot tool makes, for the other half of the engine:
// the fastest way to find a bug in src/js is to feed it script somebody else
// wrote, and until now the only ways to do that were to add a unit test or to
// load a whole page. Neither works when the input is a megabyte of minified
// code from a real site and the question is "which construct in here does the
// parser not know?".
//
// The feature that matters for that input is `-p`: parse only, and print each
// error with the source around its *offset* rather than its line. Minified
// script is one line of 200KB, so a line number locates nothing; sixty
// characters either side of the offset locates it exactly.
//
// MICROBROWSER_JS_TREEWALK=1 selects the tree-walker here as everywhere else,
// which makes this the tool for a differential question too: run the file
// twice and diff.

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "js/Interpreter.h"
#include "util/PerformanceCounters.h"
#include "js/Parser.h"
#include "js/Value.h"

namespace {

const char* kUsage =
    "usage: microbrowser_jsshell [-p] [-q] <file.js>\n"
    "       microbrowser_jsshell [-p] [-q] -e <source>\n"
    "  -p  parse only; print each syntax error with the source around it\n"
    "  -q  do not echo console output\n";

// Sixty characters either side of `offset`, control characters folded to
// spaces so a minified blob stays on one printable line.
std::string Excerpt(std::string_view source, std::size_t offset) {
  constexpr std::size_t kSpan = 60;
  const std::size_t begin = offset > kSpan ? offset - kSpan : 0;
  const std::size_t end = offset + kSpan < source.size() ? offset + kSpan : source.size();
  std::string out;
  out.reserve(end - begin);
  for (std::size_t i = begin; i < end; ++i) {
    const unsigned char c = static_cast<unsigned char>(source[i]);
    out.push_back(c < 0x20 || c == 0x7F ? ' ' : source[i]);
  }
  // A caret under the offset is worth more than the excerpt alone: in minified
  // code the token that failed and the ten around it look identical.
  out += "\n        ";
  out.append(offset - begin, ' ');
  out.push_back('^');
  return out;
}

bool ReadFile(const char* path, std::string& out) {
  std::FILE* file = std::fopen(path, "rb");
  if (file == nullptr) {
    return false;
  }
  char buffer[65536];
  std::size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    out.append(buffer, read);
  }
  std::fclose(file);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  bool parse_only = false;
  bool quiet = false;
  std::string source;
  bool have_source = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "-p") {
      parse_only = true;
    } else if (arg == "-q") {
      quiet = true;
    } else if (arg == "-e" && i + 1 < argc) {
      source = argv[++i];
      have_source = true;
    } else if (!arg.empty() && arg.front() == '-') {
      std::fputs(kUsage, stderr);
      return 2;
    } else if (!ReadFile(argv[i], source)) {
      std::fprintf(stderr, "cannot read %s\n", argv[i]);
      return 1;
    } else {
      have_source = true;
    }
  }
  if (!have_source) {
    std::fputs(kUsage, stderr);
    return 2;
  }

  if (parse_only) {
    const microbrowser::js::ParseResult result = microbrowser::js::Parse(source);
    for (const microbrowser::js::ParseError& error : result.errors) {
      std::fprintf(stderr, "line %zu, offset %zu: %s\n  near: %s\n", error.line, error.offset,
                   error.message.c_str(), Excerpt(source, error.offset).c_str());
    }
    return result.errors.empty() ? 0 : 1;
  }

  microbrowser::js::Interpreter interpreter;
  const microbrowser::js::Result result = interpreter.Run(source);
  // The counters, because the one thing this tool could not tell you was *which
  // engine ran your file*. A compile bailout is invisible from the outside -- the
  // program still runs, on the tree-walker, and the tree-walker refuses an async
  // function at the call -- so `MICROBROWSER_PERF_COUNTERS=1 jsshell file.js` is
  // now how you find out, and `js.compile_bailout_unreserved` above zero is a bug
  // in the compiler rather than a bound.
  microbrowser::util::DumpPerformanceCountersOnce();
  if (!quiet) {
    for (const std::string& line : interpreter.ConsoleOutput()) {
      std::fprintf(stdout, "%s\n", line.c_str());
    }
  }
  if (result.completion == microbrowser::js::Completion::Throw) {
    std::fprintf(stderr, "uncaught: %s\n", microbrowser::js::ToString(result.value).c_str());
    return 1;
  }
  return 0;
}
