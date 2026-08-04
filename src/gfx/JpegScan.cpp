#include "gfx/JpegScan.h"

#include <algorithm>
#include <cstdint>

namespace microbrowser::gfx {

namespace {

// A DC category above 15 cannot be extended (T.81 F.2.2.1 caps it at 11 for
// 8-bit precision).
constexpr int kMaxDcCategory = 15;

// The restart markers, RST0 through RST7. They are the only markers this half
// of the decoder knows about, because they are the only ones that appear inside
// an entropy-coded segment rather than delimiting one.
constexpr std::uint8_t kRst0 = 0xD0;
constexpr std::uint8_t kRst7 = 0xD7;

// The scan's parameters plus the one thing that changes while it runs: how many
// more blocks the current end-of-band run covers.
struct ScanState {
  int spectral_start = 0;
  int spectral_end = 63;
  int approximation_high = 0;
  int approximation_low = 0;
  int eob_run = 0;
};

// A reader over the entropy-coded segment.
//
// Two rules do all the work here. Stuffing: a 0xFF byte inside entropy-coded
// data is followed by 0x00, and the 0x00 is not data. Termination: a 0xFF
// followed by anything else is the next marker, so the segment ended — and from
// that point the reader hands out zero bits rather than reading past it. A file
// truncated mid-scan is the single most common malformed JPEG on the web, and
// it has to decode to a partial picture rather than to a crash.
class BitReader {
 public:
  BitReader(std::span<const std::byte> bytes, std::size_t position)
      : bytes_(bytes), position_(position) {}

  int Bit() {
    if (bit_count_ == 0) {
      if (!FillByte()) {
        return 0;
      }
    }
    --bit_count_;
    return (buffer_ >> bit_count_) & 1;
  }

  // Up to 16 bits, most significant first. `count` is always a Huffman-decoded
  // category here, and the caller has already bounded it.
  int Bits(int count) {
    int value = 0;
    for (int i = 0; i < count; ++i) {
      value = (value << 1) | Bit();
    }
    return value;
  }

  // T.81 F.2.2.1: a category-n value is signed, with the low half of the range
  // meaning negative.
  int Receive(int count) {
    if (count <= 0) {
      return 0;
    }
    const int value = Bits(count);
    return value < (1 << (count - 1)) ? value - (1 << count) + 1 : value;
  }

  // At a restart marker the entropy stream is byte-aligned and the reader's
  // memory of where it was is deliberately thrown away — that is the whole
  // point of a restart, and it is what makes a corrupt segment recoverable.
  bool SkipRestart() {
    bit_count_ = 0;
    at_marker_ = false;
    // Fill bytes are legal before a marker.
    while (position_ + 1 < bytes_.size() && Byte(position_) == 0xFF &&
           Byte(position_ + 1) == 0xFF) {
      ++position_;
    }
    if (position_ + 1 >= bytes_.size() || Byte(position_) != 0xFF) {
      return false;
    }
    const std::uint8_t marker = Byte(position_ + 1);
    if (marker < kRst0 || marker > kRst7) {
      return false;
    }
    position_ += 2;
    return true;
  }

  std::size_t Position() const { return position_; }

 private:
  std::uint8_t Byte(std::size_t index) const {
    return static_cast<std::uint8_t>(bytes_[index]);
  }

  bool FillByte() {
    if (at_marker_ || position_ >= bytes_.size()) {
      at_marker_ = true;
      return false;
    }
    std::uint8_t value = Byte(position_);
    if (value == 0xFF) {
      std::size_t next = position_ + 1;
      while (next < bytes_.size() && Byte(next) == 0xFF) {
        ++next;  // fill bytes
      }
      if (next >= bytes_.size() || Byte(next) != 0x00) {
        // The next marker. Stop before it so the segment scanner finds it.
        at_marker_ = true;
        return false;
      }
      position_ = next + 1;
    } else {
      ++position_;
    }
    buffer_ = value;
    bit_count_ = 8;
    return true;
  }

  std::span<const std::byte> bytes_;
  std::size_t position_ = 0;
  std::uint8_t buffer_ = 0;
  int bit_count_ = 0;
  bool at_marker_ = false;
};

// Returns the value, or -1 when the code is not in the table. An unrecognised
// code is a corrupt stream, not a value to guess at.
int HuffDecode(BitReader& reader, const JpegHuffmanTable& table) {
  std::int32_t code = 0;
  for (int length = 1; length <= 16; ++length) {
    code = (code << 1) | reader.Bit();
    if (table.max_code[static_cast<std::size_t>(length)] >= code &&
        table.min_code[static_cast<std::size_t>(length)] <= code) {
      const std::size_t index =
          static_cast<std::size_t>(table.value_index[static_cast<std::size_t>(length)] + code);
      if (index >= table.values.size()) {
        return -1;
      }
      return table.values[index];
    }
  }
  return -1;
}

std::int16_t* BlockAt(JpegComponent& component, int row, int column) {
  if (row < 0 || column < 0 || row >= component.blocks_per_column ||
      column >= component.blocks_per_line) {
    return nullptr;
  }
  const std::size_t index =
      (static_cast<std::size_t>(row) * static_cast<std::size_t>(component.blocks_per_line) +
       static_cast<std::size_t>(column)) *
      64u;
  return component.coefficients.data() + index;
}

// The DC predictor accumulates one signed difference per block, and a component
// can hold two million blocks. Each difference fits in sixteen bits and their
// sum does not fit in thirty-two, so the running total is clamped rather than
// left to overflow — signed overflow is undefined behaviour, and a file that
// reaches it is a file an attacker wrote. On any valid 8-bit JPEG the DC
// coefficient stays inside a couple of thousand and this never fires.
void AccumulateDc(JpegComponent& component, int difference) {
  component.dc_prediction =
      std::clamp(component.dc_prediction + difference, -32768, 32767);
}

bool DecodeBaselineBlock(BitReader& reader, JpegScanComponent& scan, std::int16_t* block) {
  const int category = HuffDecode(reader, *scan.dc);
  if (category < 0 || category > kMaxDcCategory) {
    return false;
  }
  AccumulateDc(*scan.component, reader.Receive(category));
  block[0] = static_cast<std::int16_t>(scan.component->dc_prediction);

  for (int k = 1; k < 64;) {
    const int rs = HuffDecode(reader, *scan.ac);
    if (rs < 0) {
      return false;
    }
    const int size = rs & 15;
    const int run = rs >> 4;
    if (size == 0) {
      if (run != 15) {
        break;  // end of block
      }
      k += 16;
      continue;
    }
    k += run;
    if (k > 63) {
      return false;
    }
    block[kJpegZigZag[static_cast<std::size_t>(k)]] =
        static_cast<std::int16_t>(reader.Receive(size));
    ++k;
  }
  return true;
}

bool DecodeDcFirst(BitReader& reader, JpegScanComponent& scan, ScanState& state,
                   std::int16_t* block) {
  const int category = HuffDecode(reader, *scan.dc);
  if (category < 0 || category > kMaxDcCategory) {
    return false;
  }
  AccumulateDc(*scan.component, reader.Receive(category));
  // At most 32767 << 13, which is well inside an int; the truncation to int16
  // is the one the coefficient array asks for and is not undefined.
  block[0] = static_cast<std::int16_t>(scan.component->dc_prediction
                                       << state.approximation_low);
  return true;
}

void DecodeDcRefine(BitReader& reader, ScanState& state, std::int16_t* block) {
  if (reader.Bit() != 0) {
    block[0] = static_cast<std::int16_t>(block[0] | (1 << state.approximation_low));
  }
}

bool DecodeAcFirst(BitReader& reader, JpegScanComponent& scan, ScanState& state,
                   std::int16_t* block) {
  if (state.eob_run > 0) {
    --state.eob_run;
    return true;
  }
  for (int k = state.spectral_start; k <= state.spectral_end;) {
    const int rs = HuffDecode(reader, *scan.ac);
    if (rs < 0) {
      return false;
    }
    const int size = rs & 15;
    const int run = rs >> 4;
    if (size == 0) {
      if (run != 15) {
        // An end-of-band run: this block and the next (2^run - 1) end here.
        state.eob_run = (1 << run) - 1;
        if (run > 0) {
          state.eob_run += reader.Bits(run);
        }
        break;
      }
      k += 16;
      continue;
    }
    k += run;
    if (k > state.spectral_end) {
      return false;
    }
    block[kJpegZigZag[static_cast<std::size_t>(k)]] =
        static_cast<std::int16_t>(reader.Receive(size) << state.approximation_low);
    ++k;
  }
  return true;
}

// T.81 G.1.2.3, transcribed rather than restructured. The awkward part is that
// a correction bit is spent on every *already nonzero* coefficient passed over,
// including the ones passed while skipping a run of zeros, and including the
// ones passed while an end-of-band run is in progress. Restructuring this loop
// into something tidier is how decoders get it wrong.
bool DecodeAcRefine(BitReader& reader, JpegScanComponent& scan, ScanState& state,
                    std::int16_t* block) {
  const int positive = 1 << state.approximation_low;
  const int negative = -(1 << state.approximation_low);
  int k = state.spectral_start;

  if (state.eob_run == 0) {
    for (; k <= state.spectral_end; ++k) {
      const int rs = HuffDecode(reader, *scan.ac);
      if (rs < 0) {
        return false;
      }
      const int size = rs & 15;
      int run = rs >> 4;
      int value = 0;
      if (size == 0) {
        if (run != 15) {
          state.eob_run = 1 << run;
          if (run > 0) {
            state.eob_run += reader.Bits(run);
          }
          break;
        }
        // run == 15: sixteen zero coefficients, correction bits included.
      } else {
        if (size != 1) {
          return false;  // refinement can only ever add one bit
        }
        value = reader.Bit() != 0 ? positive : negative;
      }
      while (k <= state.spectral_end) {
        std::int16_t& coefficient = block[kJpegZigZag[static_cast<std::size_t>(k)]];
        if (coefficient != 0) {
          if (reader.Bit() != 0 && (coefficient & positive) == 0) {
            coefficient = static_cast<std::int16_t>(coefficient +
                                                    (coefficient >= 0 ? positive : negative));
          }
        } else {
          if (run == 0) {
            if (value != 0) {
              coefficient = static_cast<std::int16_t>(value);
            }
            break;
          }
          --run;
        }
        ++k;
      }
    }
  }

  if (state.eob_run > 0) {
    for (; k <= state.spectral_end; ++k) {
      std::int16_t& coefficient = block[kJpegZigZag[static_cast<std::size_t>(k)]];
      if (coefficient != 0 && reader.Bit() != 0 && (coefficient & positive) == 0) {
        coefficient = static_cast<std::int16_t>(coefficient +
                                                (coefficient >= 0 ? positive : negative));
      }
    }
    --state.eob_run;
  }
  return true;
}

bool DecodeOneBlock(BitReader& reader, JpegScanComponent& scan, ScanState& state, bool progressive,
                    std::int16_t* block) {
  if (block == nullptr) {
    return false;
  }
  if (!progressive) {
    return DecodeBaselineBlock(reader, scan, block);
  }
  if (state.spectral_start == 0) {
    if (state.approximation_high == 0) {
      return DecodeDcFirst(reader, scan, state, block);
    }
    DecodeDcRefine(reader, state, block);
    return true;
  }
  if (state.approximation_high == 0) {
    return DecodeAcFirst(reader, scan, state, block);
  }
  return DecodeAcRefine(reader, scan, state, block);
}

}  // namespace

bool BuildJpegHuffmanTable(const std::array<std::uint8_t, 16>& counts,
                           std::vector<std::uint8_t> values, JpegHuffmanTable& table) {
  std::int32_t code = 0;
  std::int32_t index = 0;
  for (int length = 1; length <= 16; ++length) {
    const auto count = static_cast<std::int32_t>(counts[static_cast<std::size_t>(length - 1)]);
    const auto slot = static_cast<std::size_t>(length);
    // value_index[l] is chosen so that values[value_index[l] + code] is the
    // value for `code`, which is why it can be negative.
    table.value_index[slot] = index - code;
    table.min_code[slot] = code;
    code += count;
    index += count;
    table.max_code[slot] = count > 0 ? code - 1 : -1;
    if (count == 0) {
      table.min_code[slot] = 1;  // min > max, so no code of this length matches
    }
    // More codes than the length can hold means the table is not a prefix code.
    if (code > (1 << length)) {
      return false;
    }
    code <<= 1;
  }
  if (static_cast<std::size_t>(index) != values.size()) {
    return false;
  }
  table.values = std::move(values);
  table.defined = true;
  return true;
}

std::size_t DecodeJpegScan(std::span<const std::byte> bytes, std::size_t position,
                           std::span<JpegScanComponent> scan, const JpegScanParameters& params) {
  ScanState state;
  state.spectral_start = params.spectral_start;
  state.spectral_end = params.spectral_end;
  state.approximation_high = params.approximation_high;
  state.approximation_low = params.approximation_low;

  for (JpegScanComponent& component : scan) {
    component.component->dc_prediction = 0;
  }

  BitReader reader(bytes, position);
  int until_restart = params.restart_interval;
  bool stopped = false;
  for (int row = 0; row < params.rows && !stopped; ++row) {
    for (int column = 0; column < params.columns; ++column) {
      if (params.restart_interval > 0 && until_restart == 0) {
        // A restart the file promised and did not deliver is a corrupt file,
        // and continuing past it would decode the next marker as coefficients.
        if (!reader.SkipRestart()) {
          stopped = true;
          break;
        }
        until_restart = params.restart_interval;
        state.eob_run = 0;
        for (JpegScanComponent& component : scan) {
          component.component->dc_prediction = 0;
        }
      }
      if (params.restart_interval > 0) {
        --until_restart;
      }

      bool ok = true;
      if (params.interleaved) {
        for (JpegScanComponent& component : scan) {
          for (int v = 0; v < component.component->v && ok; ++v) {
            for (int h = 0; h < component.component->h && ok; ++h) {
              ok = DecodeOneBlock(reader, component, state, params.progressive,
                                  BlockAt(*component.component, row * component.component->v + v,
                                          column * component.component->h + h));
            }
          }
          if (!ok) {
            break;
          }
        }
      } else {
        ok = DecodeOneBlock(reader, scan[0], state, params.progressive,
                            BlockAt(*scan[0].component, row, column));
      }
      if (!ok) {
        stopped = true;
        break;
      }
    }
  }
  return reader.Position();
}

}  // namespace microbrowser::gfx
