// What the wire costs after it has arrived: content decoding.
//
// This file exists because there was no way to measure `util::Inflate` other
// than loading a real page, where the number is buried under a network whose
// variance is larger than the thing being measured -- seven gzip responses on
// youtube.com read anywhere between 12ms and 25ms each from run to run,
// depending on nothing that happens in this process. TD-0006 said inflate ran
// at roughly a tenth of the speed it should and could not say so more precisely
// than that, for exactly this reason.
//
// The corpus is built here rather than checked in, and that means this file
// contains a small DEFLATE *encoder*. `src/util` deliberately has none -- a
// browser has no reason to compress -- and a benchmark is not a reason to give
// it one, so the encoder lives here, is fixed-Huffman only (RFC 1951 §3.2.6),
// and knows how to emit exactly the three things the decoder's hot paths are:
// a literal, a non-overlapping match, and an overlapping run.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "BenchSupport.h"
#include "util/Inflate.h"

namespace microbrowser::bench {

namespace {

// A DEFLATE bit stream, LSB first, with Huffman codes written MSB first --
// which is the one asymmetry in the format and the usual place an encoder goes
// wrong.
class BitWriter {
 public:
  void Bits(std::uint32_t value, int count) {
    for (int i = 0; i < count; ++i) {
      Bit((value >> i) & 1u);
    }
  }

  // A Huffman code: the same bits, most significant first.
  void Code(std::uint32_t value, int count) {
    for (int i = count - 1; i >= 0; --i) {
      Bit((value >> static_cast<unsigned>(i)) & 1u);
    }
  }

  std::vector<std::byte> Take() {
    if (bit_ != 0) {
      out_.push_back(static_cast<std::byte>(partial_));
    }
    return std::move(out_);
  }

 private:
  void Bit(std::uint32_t value) {
    partial_ |= static_cast<std::uint8_t>(value << bit_);
    if (++bit_ == 8) {
      out_.push_back(static_cast<std::byte>(partial_));
      partial_ = 0;
      bit_ = 0;
    }
  }

  std::vector<std::byte> out_;
  std::uint8_t partial_ = 0;
  int bit_ = 0;
};

// One literal, in the fixed literal/length alphabet.
void WriteLiteral(BitWriter& writer, unsigned byte) {
  if (byte < 144) {
    writer.Code(0x30u + byte, 8);
  } else {
    writer.Code(0x190u + (byte - 144u), 9);
  }
}

// A match, at a `distance` of 1 or 257 and a `length` of 3..10 or 258. Those
// are exactly the lengths and distances whose symbols carry no extra bits,
// which is what keeps this encoder to a page: 3..10 are symbols 257..264 and
// 258 is symbol 285.
//
// Both distances matter and for different reasons: 257 is a plain copy and 1 is
// a run, and the decoder takes a different path for each.
void WriteMatch(BitWriter& writer, unsigned length, unsigned distance) {
  const unsigned symbol = length == 258u ? 285u : 257u + (length - 3u);
  if (symbol < 280u) {
    writer.Code(symbol - 256u, 7);
  } else {
    writer.Code(0xC0u + (symbol - 280u), 8);
  }
  if (distance == 1) {
    writer.Code(0u, 5);                            // distance symbol 0, base 1
  } else {
    writer.Code(16u, 5);                           // distance symbol 16, base 257
    writer.Bits(0u, 7);                            // its seven extra bits
  }
}

// Text with the shape a compressor meets on the web: a small vocabulary and
// long repeated runs. Deterministic, because a benchmark whose input changes
// per run produces numbers that cannot be compared across a change.
std::string SyntheticMarkup(std::size_t target_bytes) {
  static constexpr const char* kWords[] = {
      "div",   "span",  "class", "header", "content", "wrapper", "the", "and",
      "value", "style", "width", "height", "margin",  "padding", "for", "with",
  };
  std::string out;
  out.reserve(target_bytes + 128);
  std::uint32_t state = 0x1234567u;
  while (out.size() < target_bytes) {
    // xorshift, inline: this must produce the same bytes on every machine, so
    // it cannot use the platform's generator.
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    const std::size_t pick = state % 16u;
    out += "<";
    out += kWords[pick];
    out += " class=\"";
    out += kWords[(pick + 3u) % 16u];
    out += "\">";
    out += kWords[(pick + 7u) % 16u];
    out += "</";
    out += kWords[pick];
    out += ">\n";
  }
  return out;
}

struct Corpus {
  std::vector<std::byte> deflated;
  std::vector<std::byte> expected;
};

// A single fixed-Huffman block: a literal prefix long enough for the matches to
// reach back into, then an alternation of copies and runs.
//
// `short_matches` is the whole reason there are two of these. A match of 258
// bytes is two symbol decodes and a 258-byte `memcpy`, so a corpus made of them
// measures the *copy*; a match of three to ten bytes is two symbol decodes for
// a handful of bytes, so a corpus made of those measures the *Huffman decode*.
// Real compressed markup and real minified JavaScript are much closer to the
// second, and a benchmark that only had the first would have reported a fifth
// of the improvement this file exists to check.
Corpus BuildCorpus(bool short_matches) {
  const std::string text = SyntheticMarkup(64u * 1024u);
  BitWriter writer;
  writer.Bits(1u, 1);  // BFINAL
  writer.Bits(1u, 2);  // BTYPE = fixed Huffman

  Corpus corpus;
  for (const char character : text) {
    WriteLiteral(writer, static_cast<unsigned char>(character));
    corpus.expected.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
  }
  const int matches = short_matches ? 400000 : 8000;
  for (int block = 0; block < matches; ++block) {
    const unsigned distance = (block % 4 == 3) ? 1u : 257u;
    const unsigned length = short_matches ? 3u + static_cast<unsigned>(block % 8) : 258u;
    // Every fourth output is a literal, so the stream is not one shape end to
    // end -- a decoder specialised for a single symbol kind would flatter
    // itself on a corpus that only had one.
    if (block % 4 == 0) {
      WriteLiteral(writer, static_cast<unsigned>('x'));
      corpus.expected.push_back(static_cast<std::byte>('x'));
    }
    WriteMatch(writer, length, distance);
    const std::size_t from = corpus.expected.size() - distance;
    for (std::size_t i = 0; i < length; ++i) {
      corpus.expected.push_back(corpus.expected[from + i]);
    }
  }
  writer.Code(0u, 7);  // end-of-block symbol 256
  corpus.deflated = writer.Take();
  return corpus;
}

}  // namespace

void RegisterCodecBenchmarks(std::vector<Benchmark>& benchmarks) {
  // Function-local statics rather than one shared object, so a filtered run
  // builds only the corpus it is going to measure.
  static const Corpus long_matches = BuildCorpus(/*short_matches=*/false);
  static const Corpus short_matches = BuildCorpus(/*short_matches=*/true);

  const auto add = [&benchmarks](std::string_view name, const Corpus& corpus) {
    // Verified rather than trusted: an encoder written for a benchmark is
    // exactly the kind of code that emits a stream the decoder rejects on its
    // first symbol, and a benchmark measuring a decode that failed immediately
    // reports a wonderful number.
    std::vector<std::byte> check;
    if (!util::Inflate(corpus.deflated, corpus.expected.size(), check) ||
        check != corpus.expected) {
      return;  // the corpus is wrong; measuring it would be worse than not
    }
    AddBenchmark(benchmarks, name, corpus.expected.size(), "byte", [&corpus] {
      std::vector<std::byte> out;
      (void)util::Inflate(corpus.deflated, corpus.expected.size(), out);
    });
  };

  // Named for what dominates each, not for the corpus that produces it.
  add("codec/inflate-symbols", short_matches);
  add("codec/inflate-copies", long_matches);
}

}  // namespace microbrowser::bench
