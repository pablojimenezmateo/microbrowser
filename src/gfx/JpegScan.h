#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace microbrowser::gfx {

// The entropy-coded half of JPEG: a scan's bits into a component's
// coefficients.
//
// Private to gfx, and split from JpegDecoder.cpp for the reason the module lint
// gives — a file over its cap means a missing module. The seam is a real one.
// On the other side of it a file is a sequence of marked segments carrying
// tables and a frame description, which is a container problem. On this side it
// is a bit stream with no framing at all, whose end is wherever a 0xFF byte
// says it is, decoded against tables the other side supplied. The two halves
// fail differently and are read differently, and neither one needs the other's
// vocabulary.

// Coefficients arrive in zigzag order and are stored in natural order, so that
// the arithmetic never has to know the ordering exists. Both DQT and the scans
// index through this.
inline constexpr std::array<std::uint8_t, 64> kJpegZigZag = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// Canonical Huffman as T.81 Annex F builds it: per code length, the smallest
// and largest code, and where that length's values start.
struct JpegHuffmanTable {
  std::array<std::int32_t, 17> min_code{};
  std::array<std::int32_t, 17> max_code{};
  std::array<std::int32_t, 17> value_index{};
  std::vector<std::uint8_t> values;
  bool defined = false;
};

// False when the counts do not describe a prefix code, or do not account for
// exactly the values given. Both are things a file can claim and neither is
// something to decode past.
bool BuildJpegHuffmanTable(const std::array<std::uint8_t, 16>& counts,
                           std::vector<std::uint8_t> values, JpegHuffmanTable& table);

// One component's coefficients. `blocks_per_line` and `blocks_per_column` are
// the interleaved MCU grid, which is what is allocated; a non-interleaved scan
// walks a smaller grid inside it, and writing both sizes down separately is
// what keeps that from being an overrun.
struct JpegComponent {
  int id = 0;
  int h = 1;
  int v = 1;
  int quant_table = 0;
  int blocks_per_line = 0;
  int blocks_per_column = 0;
  int dc_prediction = 0;
  std::vector<std::int16_t> coefficients;
};

// One component's participation in one scan, with its tables already resolved
// and range-checked by the caller.
struct JpegScanComponent {
  JpegComponent* component = nullptr;
  const JpegHuffmanTable* dc = nullptr;
  const JpegHuffmanTable* ac = nullptr;
};

// Everything the scan header said, already validated.
struct JpegScanParameters {
  bool progressive = false;
  bool interleaved = false;
  int spectral_start = 0;
  int spectral_end = 63;
  int approximation_high = 0;
  int approximation_low = 0;
  int restart_interval = 0;
  // The grid this scan walks: MCUs when interleaved, the single component's own
  // blocks when not.
  int rows = 0;
  int columns = 0;
};

// Decodes one entropy-coded segment beginning at `position`, and returns where
// it stopped — which is the 0xFF of the next marker, or the end of the input.
//
// A corrupt or truncated scan stops the scan and not the decode: whatever
// coefficients arrived before it are a partial picture, which is what the
// format is designed to degrade into and what a browser has to show.
std::size_t DecodeJpegScan(std::span<const std::byte> bytes, std::size_t position,
                           std::span<JpegScanComponent> scan, const JpegScanParameters& params);

}  // namespace microbrowser::gfx
