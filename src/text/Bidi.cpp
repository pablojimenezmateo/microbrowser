#include "text/Bidi.h"

#include <algorithm>
#include <array>
#include <string>

#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

// UAX #9, X1 through L2. The rule numbers in the comments are the specification's, and they are there
// so that a reader can check this against the document rather than against my prose -- every rule is
// implemented in the order the specification gives it, because the rules are not independent and
// reordering two of them changes the answer.
//
// **The tables are in BidiTables.inc and generated.** See UnicodeProperties.h for why.

namespace microbrowser::text {

namespace {

// The two row types the generated table is made of. Declared here rather than in the header because
// nothing outside this file has any use for them -- the table is an implementation of
// `BidiClassOf`, not an interface.
struct BidiClassRange {
  std::uint32_t first;
  std::uint32_t last;
  BidiClass value;
};

struct BracketPair {
  std::uint32_t code;
  std::uint32_t paired;
  bool opens;
};

struct MirrorPair {
  std::uint32_t code;
  std::uint32_t mirrored;
};

#include "text/BidiTables.inc"

// The classes that X9 removes from the text: explicit embeddings, overrides, the pop, and BN. They are
// kept in the arrays -- "retaining BNs", UAX #9 §5.2 -- because the offsets have to keep matching the
// text the caller passed in, and instead marked so that the resolution rules skip them.
bool IsRemovedByX9(BidiClass c) {
  return c == BidiClass::RLE || c == BidiClass::LRE || c == BidiClass::RLO ||
         c == BidiClass::LRO || c == BidiClass::PDF || c == BidiClass::BN;
}

bool IsIsolateInitiator(BidiClass c) {
  return c == BidiClass::LRI || c == BidiClass::RLI || c == BidiClass::FSI;
}

// "NI" in the specification's shorthand: neutral or isolate formatting. Rules N1 and N2 are about
// exactly this set, and it is not the same as "neutral" -- the isolate initiators and PDI are in it.
bool IsNeutralOrIsolate(BidiClass c) {
  return c == BidiClass::B || c == BidiClass::S || c == BidiClass::WS || c == BidiClass::ON ||
         IsIsolateInitiator(c) || c == BidiClass::PDI;
}

bool IsStrong(BidiClass c) {
  return c == BidiClass::L || c == BidiClass::R || c == BidiClass::AL;
}

// The maximum explicit depth, and it is 125 rather than a round number because that is what the
// specification says. Beyond it an embedding is *ignored* rather than clamped, which is what the
// overflow counters below are for: clamping would silently reinterpret a deeply nested document.
constexpr int kMaxDepth = 125;

std::uint8_t NextOdd(std::uint8_t level) { return static_cast<std::uint8_t>((level + 1) | 1); }
std::uint8_t NextEven(std::uint8_t level) { return static_cast<std::uint8_t>((level + 2) & ~1); }

// BD9: the PDI that matches each isolate initiator, or the text length when it has none. Computed once
// for the whole line because three rules need it -- X5c, BD13, and the eos of an isolating run
// sequence -- and computing it three times is three chances to disagree.
std::vector<std::size_t> MatchingPdis(const std::vector<BidiClass>& classes) {
  std::vector<std::size_t> matching(classes.size(), classes.size());
  std::vector<std::size_t> open;
  for (std::size_t i = 0; i < classes.size(); ++i) {
    if (IsIsolateInitiator(classes[i])) {
      open.push_back(i);
    } else if (classes[i] == BidiClass::PDI && !open.empty()) {
      matching[open.back()] = i;
      open.pop_back();
    }
  }
  return matching;
}

// P2/P3, applied to a slice. Used for the paragraph itself and, by X5c, for the contents of an FSI --
// which is the same question asked about a smaller range, so it is the same function.
std::uint8_t LevelOfFirstStrong(const std::vector<std::uint32_t>& text,
                                const std::vector<BidiClass>& classes,
                                const std::vector<std::size_t>& matching, std::size_t from,
                                std::size_t to) {
  for (std::size_t i = from; i < to; ++i) {
    // P2: skip over an isolate's contents entirely. A right-to-left run *inside* an isolate says
    // nothing about the direction of the text around it -- that is what an isolate is for.
    if (IsIsolateInitiator(classes[i])) {
      i = std::min(matching[i], to);
      continue;
    }
    if (classes[i] == BidiClass::L) {
      return 0;
    }
    if (classes[i] == BidiClass::R || classes[i] == BidiClass::AL) {
      return 1;
    }
    if (classes[i] == BidiClass::B) {
      break;  // P2 stops at the first paragraph separator.
    }
  }
  (void)text;
  return 0;  // P3: no strong character at all is left-to-right.
}

// BD16's canonical equivalences. The two pairs Unicode singles out, and they are here rather than in a
// general normalization because these two are the whole list: a `〈` typed as U+2329 must pair with a
// `〉` typed as U+3009.
std::uint32_t CanonicalBracket(std::uint32_t code) {
  if (code == 0x3008) {
    return 0x2329;
  }
  if (code == 0x3009) {
    return 0x232A;
  }
  return code;
}

// One isolating run sequence (BD13): level runs joined across matched isolates, so that the text
// inside `LRI … PDI` is resolved as a unit and the text either side of it is resolved as another.
struct RunSequence {
  std::vector<std::size_t> indices;  // positions in the original text, in logical order
  std::uint8_t level = 0;
  BidiClass sos = BidiClass::L;
  BidiClass eos = BidiClass::L;
};

BidiClass DirectionOfLevel(std::uint8_t level) {
  return (level & 1) != 0 ? BidiClass::R : BidiClass::L;
}

}  // namespace

BidiClass BidiClassOf(std::uint32_t code_point) {
  // A binary search, and `L` for anything no range covers -- which is the table's default and the
  // reason it is 720 rows instead of 1,200.
  std::size_t low = 0;
  std::size_t high = std::size(kBidiClassRanges);
  while (low < high) {
    const std::size_t mid = low + (high - low) / 2;
    if (code_point < kBidiClassRanges[mid].first) {
      high = mid;
    } else if (code_point > kBidiClassRanges[mid].last) {
      low = mid + 1;
    } else {
      return kBidiClassRanges[mid].value;
    }
  }
  return BidiClass::L;
}

std::uint32_t PairedBracket(std::uint32_t code_point, bool& opens) {
  opens = false;
  std::size_t low = 0;
  std::size_t high = std::size(kBracketPairs);
  while (low < high) {
    const std::size_t mid = low + (high - low) / 2;
    if (code_point < kBracketPairs[mid].code) {
      high = mid;
    } else if (code_point > kBracketPairs[mid].code) {
      low = mid + 1;
    } else {
      opens = kBracketPairs[mid].opens;
      return kBracketPairs[mid].paired;
    }
  }
  return 0;
}

std::uint32_t MirroredGlyph(std::uint32_t code_point) {
  std::size_t low = 0;
  std::size_t high = std::size(kMirrorPairs);
  while (low < high) {
    const std::size_t mid = low + (high - low) / 2;
    if (code_point < kMirrorPairs[mid].code) {
      high = mid;
    } else if (code_point > kMirrorPairs[mid].code) {
      low = mid + 1;
    } else {
      return kMirrorPairs[mid].mirrored;
    }
  }
  return code_point;
}

std::string MirrorForRightToLeft(std::string_view utf8) {
  // Two passes, and the first one usually ends it. Every mirrorable character is at U+0028 or above
  // and the table is 428 entries, so a scan that finds nothing costs one lookup per code point and no
  // allocation at all -- which matters because this runs on every right-to-left run painted, and
  // almost none of them contain a bracket.
  std::size_t at = 0;
  bool any = false;
  while (at < utf8.size() && !any) {
    std::uint32_t code = 0;
    if (!util::DecodeUtf8(utf8, at, code)) {
      break;
    }
    any = MirroredGlyph(code) != code;
  }
  if (!any) {
    return std::string(utf8);
  }
  std::string out;
  out.reserve(utf8.size());
  at = 0;
  while (at < utf8.size()) {
    std::uint32_t code = 0;
    if (!util::DecodeUtf8(utf8, at, code)) {
      break;
    }
    util::AppendUtf8(out, MirroredGlyph(code));
  }
  util::AddPerformanceCounter(util::PerfCounterId::TextMirroredRuns);
  return out;
}

std::uint8_t ParagraphLevel(const std::vector<std::uint32_t>& text) {
  std::vector<BidiClass> classes(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    classes[i] = BidiClassOf(text[i]);
  }
  return LevelOfFirstStrong(text, classes, MatchingPdis(classes), 0, text.size());
}

std::vector<std::uint8_t> ResolveLevels(const std::vector<std::uint32_t>& text,
                                        std::uint8_t paragraph_level) {
  const std::size_t n = text.size();
  std::vector<std::uint8_t> levels(n, paragraph_level);
  if (n == 0) {
    return levels;
  }
  std::vector<BidiClass> original(n);
  for (std::size_t i = 0; i < n; ++i) {
    original[i] = BidiClassOf(text[i]);
  }
  const std::vector<std::size_t> matching = MatchingPdis(original);
  std::vector<BidiClass> classes = original;
  std::vector<bool> removed(n, false);

  // --- X1 through X8: the explicit levels ----------------------------------------------------
  //
  // A stack of (level, override, is-isolate) plus three counters. The counters are the interesting
  // part: an embedding past the depth limit is *ignored*, and the count of ignored ones is what lets
  // the matching pop be ignored too. Without them a document with 200 nested embeddings would have
  // its pops applied to the wrong entries and the text after it would be laid out at a level nobody
  // asked for.
  struct Entry {
    std::uint8_t level;
    BidiClass override_status;  // L, R, or ON meaning "no override"
    bool isolate;
  };
  std::vector<Entry> stack;
  stack.push_back({paragraph_level, BidiClass::ON, false});
  int overflow_isolate = 0;
  int overflow_embedding = 0;
  int valid_isolate = 0;

  for (std::size_t i = 0; i < n; ++i) {
    const BidiClass c = original[i];
    switch (c) {
      case BidiClass::RLE:
      case BidiClass::LRE:
      case BidiClass::RLO:
      case BidiClass::LRO: {
        levels[i] = stack.back().level;
        removed[i] = true;
        const bool rtl = c == BidiClass::RLE || c == BidiClass::RLO;
        const std::uint8_t next = rtl ? NextOdd(stack.back().level) : NextEven(stack.back().level);
        if (next <= kMaxDepth && overflow_isolate == 0 && overflow_embedding == 0) {
          const BidiClass override_status = c == BidiClass::RLO   ? BidiClass::R
                                            : c == BidiClass::LRO ? BidiClass::L
                                                                  : BidiClass::ON;
          stack.push_back({next, override_status, false});
        } else if (overflow_isolate == 0) {
          ++overflow_embedding;
        }
        break;
      }
      case BidiClass::LRI:
      case BidiClass::RLI:
      case BidiClass::FSI: {
        // X5a/X5b/X5c. An isolate initiator is *not* removed: it stays as a neutral, which is why a
        // `RLI` between two Latin words does not join them into one run.
        bool rtl = c == BidiClass::RLI;
        if (c == BidiClass::FSI) {
          // X5c: look at what is inside, by the same rule that decides a paragraph's own direction.
          const std::size_t end = std::min(matching[i], n);
          rtl = LevelOfFirstStrong(text, original, matching, i + 1, end) == 1;
        }
        levels[i] = stack.back().level;
        if (stack.back().override_status != BidiClass::ON) {
          classes[i] = stack.back().override_status;
        }
        const std::uint8_t next = rtl ? NextOdd(stack.back().level) : NextEven(stack.back().level);
        if (next <= kMaxDepth && overflow_isolate == 0 && overflow_embedding == 0) {
          ++valid_isolate;
          stack.push_back({next, BidiClass::ON, true});
        } else {
          ++overflow_isolate;
        }
        break;
      }
      case BidiClass::PDI: {
        // X6a, and the order matters: an overflowed isolate is unwound before a valid one, or a
        // document that overflowed once would pop a level it never pushed.
        if (overflow_isolate > 0) {
          --overflow_isolate;
        } else if (valid_isolate > 0) {
          overflow_embedding = 0;
          while (!stack.back().isolate) {
            stack.pop_back();
          }
          stack.pop_back();
          --valid_isolate;
        }
        levels[i] = stack.back().level;
        if (stack.back().override_status != BidiClass::ON) {
          classes[i] = stack.back().override_status;
        }
        break;
      }
      case BidiClass::PDF: {
        // X7. An isolate is *not* popped by a PDF -- that is the whole difference between an embedding
        // and an isolate, and the `!isolate` test is where it lives.
        levels[i] = stack.back().level;
        removed[i] = true;
        if (overflow_isolate > 0) {
          break;
        }
        if (overflow_embedding > 0) {
          --overflow_embedding;
        } else if (!stack.back().isolate && stack.size() >= 2) {
          stack.pop_back();
        }
        break;
      }
      case BidiClass::B: {
        // X8. One paragraph is being resolved here, so a B can only be its terminator; it takes the
        // paragraph level and everything resets.
        stack.clear();
        stack.push_back({paragraph_level, BidiClass::ON, false});
        overflow_isolate = 0;
        overflow_embedding = 0;
        valid_isolate = 0;
        levels[i] = paragraph_level;
        break;
      }
      case BidiClass::BN: {
        levels[i] = stack.back().level;
        removed[i] = true;
        break;
      }
      default: {
        // X6.
        levels[i] = stack.back().level;
        if (stack.back().override_status != BidiClass::ON) {
          classes[i] = stack.back().override_status;
        }
        break;
      }
    }
  }

  // --- BD13: the isolating run sequences -----------------------------------------------------
  //
  // Level runs over the characters X9 did not remove, then joined: a run ending in a matched isolate
  // initiator continues at that initiator's PDI. What this buys is that the text before an isolate and
  // the text after it resolve *together*, which is what makes an isolate transparent to the sentence
  // around it and an embedding not.
  std::vector<std::size_t> kept;
  kept.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    if (!removed[i]) {
      kept.push_back(i);
    }
  }
  std::vector<std::vector<std::size_t>> level_runs;
  for (const std::size_t index : kept) {
    if (level_runs.empty() || levels[level_runs.back().back()] != levels[index]) {
      level_runs.push_back({index});
    } else {
      level_runs.back().push_back(index);
    }
  }
  // Which level run each kept position starts, so the join can find the run beginning at a PDI without
  // a scan per isolate.
  std::vector<std::size_t> run_starting_at(n, level_runs.size());
  for (std::size_t r = 0; r < level_runs.size(); ++r) {
    run_starting_at[level_runs[r].front()] = r;
  }
  std::vector<bool> used(level_runs.size(), false);
  std::vector<RunSequence> sequences;
  for (std::size_t r = 0; r < level_runs.size(); ++r) {
    if (used[r]) {
      continue;
    }
    // A sequence starts at a run whose first character is not a PDI that matches an initiator --
    // because such a run is the *continuation* of a sequence that starts earlier.
    const std::size_t first = level_runs[r].front();
    if (classes[first] == BidiClass::PDI) {
      bool matched = false;
      for (std::size_t i = 0; i < n && !matched; ++i) {
        matched = IsIsolateInitiator(original[i]) && matching[i] == first;
      }
      if (matched) {
        continue;
      }
    }
    RunSequence sequence;
    std::size_t current = r;
    while (true) {
      used[current] = true;
      sequence.indices.insert(sequence.indices.end(), level_runs[current].begin(),
                              level_runs[current].end());
      const std::size_t last = level_runs[current].back();
      if (!IsIsolateInitiator(original[last]) || matching[last] >= n) {
        break;
      }
      const std::size_t next = run_starting_at[matching[last]];
      if (next >= level_runs.size() || used[next]) {
        break;
      }
      current = next;
    }
    sequence.level = levels[sequence.indices.front()];
    sequences.push_back(std::move(sequence));
  }

  // X10: sos and eos. The boundary direction is the higher of this sequence's level and the level of
  // the text on that side -- so a right-to-left sequence inside left-to-right text sees `R` on both
  // sides and its own neutrals resolve inwards rather than outwards.
  for (RunSequence& sequence : sequences) {
    const std::size_t first = sequence.indices.front();
    const std::size_t last = sequence.indices.back();
    std::uint8_t before = paragraph_level;
    for (std::size_t i = first; i-- > 0;) {
      if (!removed[i]) {
        before = levels[i];
        break;
      }
    }
    std::uint8_t after = paragraph_level;
    // An unmatched isolate initiator at the end takes the paragraph level rather than what follows --
    // there is nothing that "follows" inside an isolate that never closed.
    if (!(IsIsolateInitiator(original[last]) && matching[last] >= n)) {
      for (std::size_t i = last + 1; i < n; ++i) {
        if (!removed[i]) {
          after = levels[i];
          break;
        }
      }
    }
    sequence.sos = DirectionOfLevel(std::max(sequence.level, before));
    sequence.eos = DirectionOfLevel(std::max(sequence.level, after));
  }

  // --- W1-W7, N0-N2, I1-I2, per isolating run sequence ---------------------------------------
  for (const RunSequence& sequence : sequences) {
    const std::vector<std::size_t>& in = sequence.indices;
    const std::size_t count = in.size();
    const auto class_at = [&](std::size_t k) -> BidiClass& { return classes[in[k]]; };

    // W1: NSM takes the class of what precedes it -- and ON after an isolate initiator or PDI, because
    // a combining mark cannot combine across an isolate boundary.
    for (std::size_t k = 0; k < count; ++k) {
      if (class_at(k) != BidiClass::NSM) {
        continue;
      }
      if (k == 0) {
        class_at(k) = sequence.sos;
      } else {
        const BidiClass previous = class_at(k - 1);
        class_at(k) = IsIsolateInitiator(previous) || previous == BidiClass::PDI ? BidiClass::ON
                                                                                : previous;
      }
    }
    // W2: a European number after an Arabic letter is an Arabic number. This is why the rules are
    // ordered: W3 turns AL into R, so W2 has to look at AL before it stops existing.
    {
      BidiClass last_strong = sequence.sos;
      for (std::size_t k = 0; k < count; ++k) {
        if (IsStrong(class_at(k))) {
          last_strong = class_at(k);
        } else if (class_at(k) == BidiClass::EN && last_strong == BidiClass::AL) {
          class_at(k) = BidiClass::AN;
        }
      }
    }
    // W3.
    for (std::size_t k = 0; k < count; ++k) {
      if (class_at(k) == BidiClass::AL) {
        class_at(k) = BidiClass::R;
      }
    }
    // W4: a single separator between two numbers of the same kind joins them. `1,000` and `1.000`.
    for (std::size_t k = 1; k + 1 < count; ++k) {
      const BidiClass previous = class_at(k - 1);
      const BidiClass next = class_at(k + 1);
      if (class_at(k) == BidiClass::ES && previous == BidiClass::EN && next == BidiClass::EN) {
        class_at(k) = BidiClass::EN;
      } else if (class_at(k) == BidiClass::CS && previous == next &&
                 (previous == BidiClass::EN || previous == BidiClass::AN)) {
        class_at(k) = previous;
      }
    }
    // W5: a run of terminators adjacent to a European number joins it -- `$1` and `1%` on either side.
    for (std::size_t k = 0; k < count; ++k) {
      if (class_at(k) != BidiClass::ET) {
        continue;
      }
      std::size_t end = k;
      while (end < count && class_at(end) == BidiClass::ET) {
        ++end;
      }
      const bool before = k > 0 && class_at(k - 1) == BidiClass::EN;
      const bool after = end < count && class_at(end) == BidiClass::EN;
      if (before || after) {
        for (std::size_t j = k; j < end; ++j) {
          class_at(j) = BidiClass::EN;
        }
      }
      k = end - 1;
    }
    // W6: whatever separators and terminators are left are neutral.
    for (std::size_t k = 0; k < count; ++k) {
      if (class_at(k) == BidiClass::ET || class_at(k) == BidiClass::ES ||
          class_at(k) == BidiClass::CS) {
        class_at(k) = BidiClass::ON;
      }
    }
    // W7: a European number after left-to-right text is left-to-right.
    {
      BidiClass last_strong = sequence.sos;
      for (std::size_t k = 0; k < count; ++k) {
        if (class_at(k) == BidiClass::L || class_at(k) == BidiClass::R) {
          last_strong = class_at(k);
        } else if (class_at(k) == BidiClass::EN && last_strong == BidiClass::L) {
          class_at(k) = BidiClass::L;
        }
      }
    }

    // N0: paired brackets. BD16 finds the pairs; N0 gives both brackets of a pair the *same*
    // direction, which is the rule that makes `(hello)` in Hebrew text keep its parentheses around the
    // word instead of one on each side of the sentence.
    {
      struct Opener {
        std::uint32_t closer;
        std::size_t position;  // index within the sequence
      };
      std::vector<Opener> openers;
      std::vector<std::pair<std::size_t, std::size_t>> pairs;
      for (std::size_t k = 0; k < count; ++k) {
        if (class_at(k) != BidiClass::ON) {
          continue;
        }
        bool opens = false;
        const std::uint32_t paired = PairedBracket(text[in[k]], opens);
        if (paired == 0) {
          continue;
        }
        if (opens) {
          // BD16's stack is 63 deep and overflowing it *stops* the pairing rather than dropping one
          // entry, because a partially-paired document resolves inconsistently.
          if (openers.size() >= 63) {
            openers.clear();
            break;
          }
          openers.push_back({CanonicalBracket(paired), k});
          continue;
        }
        const std::uint32_t closer = CanonicalBracket(text[in[k]]);
        for (std::size_t o = openers.size(); o-- > 0;) {
          if (openers[o].closer == closer) {
            pairs.emplace_back(openers[o].position, k);
            openers.resize(o);
            break;
          }
        }
      }
      std::sort(pairs.begin(), pairs.end());
      const BidiClass embedding = DirectionOfLevel(sequence.level);
      const BidiClass opposite = embedding == BidiClass::L ? BidiClass::R : BidiClass::L;
      // EN and AN count as R here, which is stated in N0 and is easy to miss: a number inside brackets
      // in Hebrew text is enough to make the brackets right-to-left.
      const auto strong_of = [&](BidiClass c) {
        if (c == BidiClass::L) {
          return BidiClass::L;
        }
        if (c == BidiClass::R || c == BidiClass::EN || c == BidiClass::AN) {
          return BidiClass::R;
        }
        return BidiClass::ON;
      };
      for (const auto& [open_at, close_at] : pairs) {
        bool has_embedding = false;
        bool has_opposite = false;
        for (std::size_t k = open_at + 1; k < close_at; ++k) {
          const BidiClass strong = strong_of(class_at(k));
          if (strong == embedding) {
            has_embedding = true;
            break;
          }
          if (strong == opposite) {
            has_opposite = true;
          }
        }
        BidiClass resolved = BidiClass::ON;
        if (has_embedding) {
          resolved = embedding;
        } else if (has_opposite) {
          // The context before the bracket decides. If it agrees with what is inside, the brackets go
          // with it; otherwise they follow the embedding, which keeps them attached to the sentence.
          BidiClass context = sequence.sos;
          for (std::size_t k = open_at; k-- > 0;) {
            const BidiClass strong = strong_of(class_at(k));
            if (strong != BidiClass::ON) {
              context = strong;
              break;
            }
          }
          resolved = context == opposite ? opposite : embedding;
        }
        if (resolved == BidiClass::ON) {
          continue;  // Nothing strong inside: the brackets stay neutral and N1/N2 will place them.
        }
        class_at(open_at) = resolved;
        class_at(close_at) = resolved;
        // And the combining marks that followed either bracket before W1 rewrote them. Without this a
        // mark on a bracket separates from it and lands at the other end of the line.
        for (const std::size_t bracket : {open_at, close_at}) {
          for (std::size_t k = bracket + 1; k < count && original[in[k]] == BidiClass::NSM; ++k) {
            class_at(k) = resolved;
          }
        }
      }
    }

    // N1: neutrals between two strongs of the same direction take it. N2: the rest take the embedding
    // direction. One pass, because N2 is what N1 falls back to.
    for (std::size_t k = 0; k < count; ++k) {
      if (!IsNeutralOrIsolate(class_at(k))) {
        continue;
      }
      std::size_t end = k;
      while (end < count && IsNeutralOrIsolate(class_at(end))) {
        ++end;
      }
      const auto side = [&](std::size_t at, bool before) {
        if (before) {
          if (at == 0) {
            return sequence.sos;
          }
          const BidiClass c = class_at(at - 1);
          return c == BidiClass::EN || c == BidiClass::AN ? BidiClass::R : c;
        }
        if (at >= count) {
          return sequence.eos;
        }
        const BidiClass c = class_at(at);
        return c == BidiClass::EN || c == BidiClass::AN ? BidiClass::R : c;
      };
      const BidiClass left = side(k, true);
      const BidiClass right = side(end, false);
      const BidiClass resolved = left == right ? left : DirectionOfLevel(sequence.level);
      for (std::size_t j = k; j < end; ++j) {
        class_at(j) = resolved;
      }
      k = end - 1;
    }

    // I1 and I2: the resolved classes become levels. This is where a number inside right-to-left text
    // gets a level two higher than the text around it -- which is what draws `123` left-to-right
    // inside a right-to-left sentence.
    for (std::size_t k = 0; k < count; ++k) {
      const std::size_t at = in[k];
      const BidiClass c = classes[at];
      if ((levels[at] & 1) == 0) {
        if (c == BidiClass::R) {
          levels[at] = static_cast<std::uint8_t>(levels[at] + 1);
        } else if (c == BidiClass::AN || c == BidiClass::EN) {
          levels[at] = static_cast<std::uint8_t>(levels[at] + 2);
        }
      } else if (c == BidiClass::L || c == BidiClass::EN || c == BidiClass::AN) {
        levels[at] = static_cast<std::uint8_t>(levels[at] + 1);
      }
    }
  }

  // §5.2: the characters X9 removed take the level of what precedes them, so that they never split a
  // run. They are invisible, and a run split by an invisible character is two shaped runs where one
  // was correct -- which breaks an Arabic ligature across a zero-width joiner.
  for (std::size_t i = 0; i < n; ++i) {
    if (removed[i]) {
      levels[i] = i == 0 ? paragraph_level : levels[i - 1];
    }
  }

  // --- L1: the levels that reset to the paragraph's -------------------------------------------
  //
  // Separators, and the trailing whitespace before them or at the end of the line. **Using the
  // *original* classes**, which is stated in the rule and is the point of it: the whitespace at the
  // end of a right-to-left line has been resolved to some level by now, and what a reader needs is for
  // it to hang off the paragraph's own edge rather than to be reordered into the middle.
  const auto resettable = [&](std::size_t i) {
    return original[i] == BidiClass::WS || IsIsolateInitiator(original[i]) ||
           original[i] == BidiClass::PDI || IsRemovedByX9(original[i]);
  };
  for (std::size_t i = 0; i < n; ++i) {
    if (original[i] != BidiClass::S && original[i] != BidiClass::B) {
      continue;
    }
    levels[i] = paragraph_level;
    for (std::size_t j = i; j-- > 0 && resettable(j);) {
      levels[j] = paragraph_level;
    }
  }
  for (std::size_t i = n; i-- > 0 && resettable(i);) {
    levels[i] = paragraph_level;
  }
  return levels;
}

std::vector<BidiRun> ResolveVisualRuns(const std::vector<std::uint32_t>& text,
                                       std::uint8_t paragraph_level) {
  const std::vector<std::uint8_t> levels = ResolveLevels(text, paragraph_level);
  std::vector<BidiRun> runs;
  for (std::size_t i = 0; i < levels.size(); ++i) {
    if (!runs.empty() && runs.back().level == levels[i]) {
      ++runs.back().length;
      continue;
    }
    BidiRun run;
    run.start = i;
    run.length = 1;
    run.level = levels[i];
    run.right_to_left = (levels[i] & 1) != 0;
    runs.push_back(run);
  }
  if (runs.empty()) {
    return runs;
  }
  // L2: from the highest level down to the lowest odd level, reverse every contiguous stretch of runs
  // at that level or above. Applied at *run* granularity rather than per character, which is the same
  // reversal -- a run is by construction all one level -- and leaves the runs themselves as slices of
  // logical text, which is what a shaper needs.
  std::uint8_t highest = 0;
  std::uint8_t lowest_odd = 0xFF;
  for (const BidiRun& run : runs) {
    highest = std::max(highest, run.level);
    if ((run.level & 1) != 0) {
      lowest_odd = std::min(lowest_odd, run.level);
    }
  }
  for (std::uint8_t level = highest; lowest_odd != 0xFF && level >= lowest_odd; --level) {
    std::size_t at = 0;
    while (at < runs.size()) {
      if (runs[at].level < level) {
        ++at;
        continue;
      }
      std::size_t end = at;
      while (end < runs.size() && runs[end].level >= level) {
        ++end;
      }
      std::reverse(runs.begin() + static_cast<std::ptrdiff_t>(at),
                   runs.begin() + static_cast<std::ptrdiff_t>(end));
      at = end;
    }
  }
  util::AddPerformanceCounter(util::PerfCounterId::TextBidiLines);
  return runs;
}

bool NeedsBidi(std::string_view utf8) {
  // Two passes and the first one usually answers. Every character that can require the algorithm is at
  // U+0590 or above, whose UTF-8 lead byte is 0xD6 or greater -- so a document of Latin, Greek,
  // Cyrillic, CJK or emoji is rejected by a byte comparison with no decoding at all. Hacker News and
  // every English page are this case, which is what keeps bidi from costing them anything.
  bool possible = false;
  for (const char c : utf8) {
    if (static_cast<unsigned char>(c) >= 0xD6) {
      possible = true;
      break;
    }
  }
  if (!possible) {
    return false;
  }
  std::size_t at = 0;
  while (at < utf8.size()) {
    std::uint32_t code = 0;
    if (!util::DecodeUtf8(utf8, at, code)) {
      // Ill-formed bytes cannot reach here -- the decoder in src/html produced this string -- but a
      // decode that fails must not loop, so it ends the scan.
      break;
    }
    switch (BidiClassOf(code)) {
      case BidiClass::R:
      case BidiClass::AL:
      case BidiClass::AN:
      case BidiClass::RLE:
      case BidiClass::LRE:
      case BidiClass::RLO:
      case BidiClass::LRO:
      case BidiClass::PDF:
      case BidiClass::LRI:
      case BidiClass::RLI:
      case BidiClass::FSI:
      case BidiClass::PDI:
        return true;
      default:
        break;
    }
  }
  return false;
}

}  // namespace microbrowser::text
