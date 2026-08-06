// Runs Unicode's own bidi conformance data against src/text/Bidi.cpp.
//
// ADR 0025 §3. **This exists because hand-written tests cannot check a bidi implementation.** The
// algorithm has twenty-odd interacting rules; a test I write exercises the cases I thought of, and
// the cases I thought of are the ones I got right. Unicode ships 861,948 cases that between them
// cover every rule interaction, and they are the only reason to believe this code.
//
// The data is not vendored -- it is 6MB and versioned upstream -- so this reads it from a directory,
// the same arrangement tools/unicode/generate.py uses and for the same reasons:
//
//     curl -O https://www.unicode.org/Public/15.1.0/ucd/BidiTest.txt
//     curl -O https://www.unicode.org/Public/15.1.0/ucd/BidiCharacterTest.txt
//     ./build/microbrowser/microbrowser_bidiconf <directory>
//
// Two files, two shapes. BidiCharacterTest.txt gives actual code points, an explicit paragraph
// direction, the expected levels and the expected visual order. BidiTest.txt gives *class names* --
// so running it turns each name into a representative code point and therefore checks the generated
// class table and the algorithm together, which is the pair that has to agree.
//
// Every case checks three answers, because they fail independently: the paragraph level from P2/P3,
// the resolved level per character, and the visual order after L1 and L2. A `x` in the expected
// levels means a character X9 removed, whose level is unspecified -- skipped rather than guessed.

#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "text/Bidi.h"

namespace {

using microbrowser::text::BidiClass;
using microbrowser::text::BidiRun;

std::vector<std::string> Split(const std::string& text, char separator) {
  std::vector<std::string> out;
  std::stringstream stream(text);
  std::string part;
  while (std::getline(stream, part, separator)) {
    out.push_back(part);
  }
  return out;
}

std::vector<int> Numbers(const std::string& text) {
  std::vector<int> out;
  std::stringstream stream(text);
  std::string part;
  while (stream >> part) {
    out.push_back(part == "x" ? -1 : std::atoi(part.c_str()));
  }
  return out;
}

std::string Trimmed(std::string text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
    text.pop_back();
  }
  return text;
}

// The visual order of the positions the expected data names, from the runs this browser produced.
// A right-to-left run's own characters are visited back to front: the run is a slice of *logical*
// text, which is what a shaper needs, and its direction says which end is painted first.
std::vector<int> VisualOrder(const std::vector<BidiRun>& runs, const std::vector<int>& levels) {
  std::vector<int> order;
  for (const BidiRun& run : runs) {
    for (std::size_t k = 0; k < run.length; ++k) {
      const std::size_t at = run.right_to_left ? run.start + run.length - 1 - k : run.start + k;
      if (at < levels.size() && levels[at] >= 0) {
        order.push_back(static_cast<int>(at));
      }
    }
  }
  return order;
}

struct Failures {
  long cases = 0;
  long paragraph = 0;
  long levels = 0;
  long order = 0;
  std::string first;

  long Total() const { return paragraph + levels + order; }
};

// One case: the text, the paragraph level to use, and what the file says should come out.
void CheckOne(const std::vector<std::uint32_t>& text, std::uint8_t paragraph_level,
              const std::vector<int>& want_levels, const std::vector<int>& want_order,
              const std::string& source, Failures& failures) {
  ++failures.cases;
  const std::vector<std::uint8_t> levels =
      microbrowser::text::ResolveLevels(text, paragraph_level);
  bool ok = levels.size() == want_levels.size();
  for (std::size_t i = 0; ok && i < levels.size(); ++i) {
    if (want_levels[i] >= 0 && levels[i] != want_levels[i]) {
      ok = false;
    }
  }
  if (!ok) {
    ++failures.levels;
    if (failures.first.empty()) {
      failures.first = "levels: " + source + "\n   got:";
      for (const std::uint8_t level : levels) {
        failures.first += " " + std::to_string(static_cast<int>(level));
      }
    }
    return;
  }
  const std::vector<int> got =
      VisualOrder(microbrowser::text::ResolveVisualRuns(text, paragraph_level), want_levels);
  if (got != want_order) {
    ++failures.order;
    if (failures.first.empty()) {
      failures.first = "order: " + source + "\n   got:";
      for (const int at : got) {
        failures.first += " " + std::to_string(at);
      }
    }
  }
}

void RunCharacterTest(const std::string& path, Failures& failures) {
  FILE* file = std::fopen(path.c_str(), "r");
  if (file == nullptr) {
    std::printf("cannot open %s\n", path.c_str());
    ++failures.levels;
    return;
  }
  std::string line(1 << 16, '\0');
  while (std::fgets(line.data(), static_cast<int>(line.size()), file) != nullptr) {
    const std::string text = Trimmed(line.c_str());
    if (text.empty() || text[0] == '#') {
      continue;
    }
    const std::vector<std::string> fields = Split(text, ';');
    if (fields.size() < 5) {
      continue;
    }
    std::vector<std::uint32_t> codes;
    {
      std::stringstream stream(fields[0]);
      std::string part;
      while (stream >> part) {
        codes.push_back(static_cast<std::uint32_t>(std::strtol(part.c_str(), nullptr, 16)));
      }
    }
    const int direction = std::atoi(fields[1].c_str());
    const int want_paragraph = std::atoi(fields[2].c_str());
    // Direction 2 is "auto", which is the only case that tests P2/P3 -- and it is tested separately,
    // because a wrong paragraph level makes every level below it wrong and the report useless.
    std::uint8_t paragraph_level = static_cast<std::uint8_t>(want_paragraph);
    if (direction == 2) {
      paragraph_level = microbrowser::text::ParagraphLevel(codes);
      if (paragraph_level != want_paragraph) {
        ++failures.cases;
        ++failures.paragraph;
        if (failures.first.empty()) {
          failures.first = "paragraph level: " + text;
        }
        continue;
      }
    }
    CheckOne(codes, paragraph_level, Numbers(fields[3]), Numbers(fields[4]), text, failures);
  }
  std::fclose(file);
}

void RunClassTest(const std::string& path, Failures& failures) {
  static const std::map<std::string, BidiClass> kNames = {
      {"L", BidiClass::L},     {"R", BidiClass::R},     {"AL", BidiClass::AL},
      {"EN", BidiClass::EN},   {"ES", BidiClass::ES},   {"ET", BidiClass::ET},
      {"AN", BidiClass::AN},   {"CS", BidiClass::CS},   {"NSM", BidiClass::NSM},
      {"BN", BidiClass::BN},   {"B", BidiClass::B},     {"S", BidiClass::S},
      {"WS", BidiClass::WS},   {"ON", BidiClass::ON},   {"LRE", BidiClass::LRE},
      {"RLE", BidiClass::RLE}, {"LRO", BidiClass::LRO}, {"RLO", BidiClass::RLO},
      {"PDF", BidiClass::PDF}, {"LRI", BidiClass::LRI}, {"RLI", BidiClass::RLI},
      {"FSI", BidiClass::FSI}, {"PDI", BidiClass::PDI}};
  // A representative code point per class, found by asking the table -- so this half exercises the
  // generated ranges as well as the rules. A class with no code point at all would mean the table is
  // missing a value the rules name, which is worth failing over rather than skipping.
  std::map<std::string, std::uint32_t> sample;
  for (const auto& [name, value] : kNames) {
    for (std::uint32_t code = 1; code < 0x11000; ++code) {
      if (microbrowser::text::BidiClassOf(code) == value) {
        sample[name] = code;
        break;
      }
    }
    if (sample.find(name) == sample.end()) {
      std::printf("no code point has bidi class %s\n", name.c_str());
      ++failures.levels;
      return;
    }
  }

  FILE* file = std::fopen(path.c_str(), "r");
  if (file == nullptr) {
    std::printf("cannot open %s\n", path.c_str());
    ++failures.levels;
    return;
  }
  std::string line(1 << 16, '\0');
  std::vector<int> want_levels;
  std::vector<int> want_order;
  while (std::fgets(line.data(), static_cast<int>(line.size()), file) != nullptr) {
    const std::string text = Trimmed(line.c_str());
    if (text.empty() || text[0] == '#') {
      continue;
    }
    if (text.rfind("@Levels:", 0) == 0) {
      want_levels = Numbers(text.substr(8));
      continue;
    }
    if (text.rfind("@Reorder:", 0) == 0) {
      want_order = Numbers(text.substr(9));
      continue;
    }
    const std::size_t semicolon = text.find(';');
    if (semicolon == std::string::npos) {
      continue;
    }
    std::vector<std::uint32_t> codes;
    for (const std::string& name : Split(text.substr(0, semicolon), ' ')) {
      if (!name.empty()) {
        codes.push_back(sample[name]);
      }
    }
    // A bitset: 1 means auto, 2 left-to-right, 4 right-to-left. Each set bit is a case.
    const int bits = std::atoi(text.substr(semicolon + 1).c_str());
    for (int which = 0; which < 3; ++which) {
      if ((bits & (1 << which)) == 0) {
        continue;
      }
      const std::uint8_t level = which == 0 ? microbrowser::text::ParagraphLevel(codes)
                                            : static_cast<std::uint8_t>(which == 1 ? 0 : 1);
      CheckOne(codes, level, want_levels, want_order,
               text + " (dir " + std::to_string(which) + ")", failures);
    }
  }
  std::fclose(file);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::printf("usage: microbrowser_bidiconf <directory with BidiTest.txt "
                "and BidiCharacterTest.txt>\n");
    return 2;
  }
  const std::string directory(argv[1]);
  Failures failures;
  RunCharacterTest(directory + "/BidiCharacterTest.txt", failures);
  RunClassTest(directory + "/BidiTest.txt", failures);
  std::printf("%ld cases: %ld paragraph-level, %ld level, %ld order failures\n", failures.cases,
              failures.paragraph, failures.levels, failures.order);
  if (!failures.first.empty()) {
    std::printf("first failure:\n  %s\n", failures.first.c_str());
  }
  return failures.Total() == 0 ? 0 : 1;
}
