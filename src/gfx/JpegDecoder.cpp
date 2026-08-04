#include "gfx/JpegDecoder.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "gfx/JpegPixels.h"
#include "gfx/JpegScan.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::gfx {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// Segment markers, by the names T.81 gives them.
constexpr std::uint8_t kSoi = 0xD8;
constexpr std::uint8_t kEoi = 0xD9;
constexpr std::uint8_t kSof0 = 0xC0;
constexpr std::uint8_t kSof1 = 0xC1;
constexpr std::uint8_t kSof2 = 0xC2;
constexpr std::uint8_t kDht = 0xC4;
constexpr std::uint8_t kDqt = 0xDB;
constexpr std::uint8_t kDri = 0xDD;
constexpr std::uint8_t kSos = 0xDA;
constexpr std::uint8_t kApp14 = 0xEE;
constexpr std::uint8_t kRst0 = 0xD0;
constexpr std::uint8_t kRst7 = 0xD7;

// The coefficient storage a progressive decoder cannot avoid: every scan
// refines coefficients that a later scan reads back, so the whole frame is in
// memory in the frequency domain before a single pixel exists. That is the
// price of progressive and it is why this bound exists separately from the
// pixel bound — a 64-megapixel image with no chroma subsampling would be three
// coefficients per pixel and half a gigabyte before the bitmap. Refused.
constexpr std::uint64_t kMaxCoefficients = kMaxImagePixels * 2ull;

struct Frame {
  bool progressive = false;
  int width = 0;
  int height = 0;
  int max_h = 1;
  int max_v = 1;
  int mcus_per_line = 0;
  int mcus_per_column = 0;
  std::vector<JpegComponent> components;
  // One rendered plane per component, filled once every scan has been read.
  std::vector<std::vector<std::uint8_t>> samples;
};

// --- The decoder -------------------------------------------------------------

class Decoder {
 public:
  explicit Decoder(std::span<const std::byte> bytes) : bytes_(bytes) {}

  const char* Run(Image& out);

 private:
  std::uint8_t Byte(std::size_t index) const {
    return static_cast<std::uint8_t>(bytes_[index]);
  }
  int ReadU16(std::size_t index) const {
    return (static_cast<int>(Byte(index)) << 8) | static_cast<int>(Byte(index + 1));
  }

  const char* ReadSegments();
  const char* ReadQuantizationTables(std::size_t start, std::size_t end);
  const char* ReadHuffmanTables(std::size_t start, std::size_t end);
  const char* ReadFrameHeader(std::size_t start, std::size_t end, bool progressive);
  const char* ReadScan(std::size_t start, std::size_t end);

  std::span<const std::byte> bytes_;
  std::size_t position_ = 0;
  Frame frame_;
  std::array<std::array<std::uint16_t, 64>, 4> quantization_{};
  std::array<bool, 4> quantization_defined_{};
  std::array<JpegHuffmanTable, 4> dc_tables_{};
  std::array<JpegHuffmanTable, 4> ac_tables_{};
  int restart_interval_ = 0;
  bool adobe_rgb_ = false;
  bool saw_frame_ = false;
  bool saw_scan_ = false;
};

const char* Decoder::ReadQuantizationTables(std::size_t start, std::size_t end) {
  std::size_t at = start;
  while (at < end) {
    const std::uint8_t spec = Byte(at++);
    const int precision = spec >> 4;
    const std::size_t slot = spec & 0x0F;
    if (slot >= 4 || precision > 1) {
      return "bad quantisation table selector";
    }
    const std::size_t needed = precision == 0 ? 64u : 128u;
    if (end - at < needed) {
      return "truncated quantisation table";
    }
    for (std::size_t i = 0; i < 64; ++i) {
      const int value = precision == 0 ? Byte(at + i) : ReadU16(at + i * 2);
      // A zero divisor would be a division by zero at dequantisation time.
      quantization_[slot][kJpegZigZag[i]] = static_cast<std::uint16_t>(value == 0 ? 1 : value);
    }
    quantization_defined_[slot] = true;
    at += needed;
  }
  return nullptr;
}

const char* Decoder::ReadHuffmanTables(std::size_t start, std::size_t end) {
  std::size_t at = start;
  while (at < end) {
    if (end - at < 17) {
      return "truncated Huffman table";
    }
    const std::uint8_t spec = Byte(at++);
    const std::size_t klass = spec >> 4;
    const std::size_t slot = spec & 0x0F;
    if (klass >= 2 || slot >= 4) {
      return "bad Huffman table selector";
    }
    std::array<std::uint8_t, 16> counts{};
    std::size_t total = 0;
    for (std::size_t i = 0; i < 16; ++i) {
      counts[i] = Byte(at + i);
      total += counts[i];
    }
    at += 16;
    if (total > 256 || end - at < total) {
      return "truncated Huffman table";
    }
    std::vector<std::uint8_t> values(total);
    for (std::size_t i = 0; i < total; ++i) {
      values[i] = Byte(at + i);
    }
    at += total;
    JpegHuffmanTable& table = klass == 0 ? dc_tables_[slot] : ac_tables_[slot];
    table = JpegHuffmanTable{};
    if (!BuildJpegHuffmanTable(counts, std::move(values), table)) {
      return "Huffman table is not a prefix code";
    }
  }
  return nullptr;
}

const char* Decoder::ReadFrameHeader(std::size_t start, std::size_t end, bool progressive) {
  if (saw_frame_) {
    return "more than one frame";
  }
  if (end - start < 6) {
    return "truncated frame header";
  }
  if (Byte(start) != 8) {
    return "only 8-bit sample precision is supported";
  }
  frame_.progressive = progressive;
  frame_.height = ReadU16(start + 1);
  frame_.width = ReadU16(start + 3);
  const int count = Byte(start + 5);
  if (frame_.width <= 0 || frame_.height <= 0) {
    return "frame has no pixels";
  }
  // Before any allocation, as ADR 0023 §2 requires. Every product here is
  // computed in 64 bits whatever the operands look like.
  if (static_cast<std::uint64_t>(frame_.width) * static_cast<std::uint64_t>(frame_.height) >
      kMaxImagePixels) {
    return "image exceeds the decoded pixel bound";
  }
  if (count != 1 && count != 3) {
    return "only one- and three-component frames are supported";
  }
  if (end - start < 6 + static_cast<std::size_t>(count) * 3) {
    return "truncated frame header";
  }

  frame_.components.resize(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    const std::size_t at = start + 6 + static_cast<std::size_t>(i) * 3;
    JpegComponent& component = frame_.components[static_cast<std::size_t>(i)];
    component.id = Byte(at);
    component.h = Byte(at + 1) >> 4;
    component.v = Byte(at + 1) & 0x0F;
    component.quant_table = Byte(at + 2);
    if (component.h < 1 || component.h > 4 || component.v < 1 || component.v > 4) {
      return "bad component sampling factor";
    }
    if (component.quant_table >= 4) {
      return "bad component quantisation selector";
    }
    for (int j = 0; j < i; ++j) {
      if (frame_.components[static_cast<std::size_t>(j)].id == component.id) {
        return "duplicate component id";
      }
    }
    frame_.max_h = std::max(frame_.max_h, component.h);
    frame_.max_v = std::max(frame_.max_v, component.v);
  }

  frame_.mcus_per_line = (frame_.width + frame_.max_h * 8 - 1) / (frame_.max_h * 8);
  frame_.mcus_per_column = (frame_.height + frame_.max_v * 8 - 1) / (frame_.max_v * 8);

  std::uint64_t coefficients = 0;
  for (JpegComponent& component : frame_.components) {
    component.blocks_per_line = frame_.mcus_per_line * component.h;
    component.blocks_per_column = frame_.mcus_per_column * component.v;
    coefficients += static_cast<std::uint64_t>(component.blocks_per_line) *
                    static_cast<std::uint64_t>(component.blocks_per_column) * 64ull;
  }
  if (coefficients > kMaxCoefficients) {
    return "image exceeds the coefficient storage bound";
  }
  for (JpegComponent& component : frame_.components) {
    component.coefficients.assign(static_cast<std::size_t>(component.blocks_per_line) *
                                      static_cast<std::size_t>(component.blocks_per_column) * 64u,
                                  0);
  }
  saw_frame_ = true;
  return nullptr;
}

const char* Decoder::ReadScan(std::size_t start, std::size_t end) {
  if (!saw_frame_) {
    return "scan before frame";
  }
  if (end - start < 1) {
    return "truncated scan header";
  }
  const int count = Byte(start);
  if (count < 1 || count > static_cast<int>(frame_.components.size())) {
    return "bad scan component count";
  }
  if (end - start < 1 + static_cast<std::size_t>(count) * 2 + 3) {
    return "truncated scan header";
  }

  std::array<JpegScanComponent, 3> scan{};
  for (int i = 0; i < count; ++i) {
    const std::size_t at = start + 1 + static_cast<std::size_t>(i) * 2;
    const int id = Byte(at);
    const std::size_t dc_slot = Byte(at + 1) >> 4;
    const std::size_t ac_slot = Byte(at + 1) & 0x0F;
    if (dc_slot >= 4 || ac_slot >= 4) {
      return "bad scan table selector";
    }
    JpegComponent* found = nullptr;
    for (JpegComponent& component : frame_.components) {
      if (component.id == id) {
        found = &component;
      }
    }
    if (found == nullptr) {
      return "scan names a component the frame does not have";
    }
    scan[static_cast<std::size_t>(i)].component = found;
    scan[static_cast<std::size_t>(i)].dc = &dc_tables_[dc_slot];
    scan[static_cast<std::size_t>(i)].ac = &ac_tables_[ac_slot];
  }

  JpegScanParameters params;
  params.progressive = frame_.progressive;
  params.restart_interval = restart_interval_;
  const std::size_t spectral = start + 1 + static_cast<std::size_t>(count) * 2;
  params.spectral_start = Byte(spectral);
  params.spectral_end = Byte(spectral + 1);
  params.approximation_high = Byte(spectral + 2) >> 4;
  params.approximation_low = Byte(spectral + 2) & 0x0F;
  if (!frame_.progressive) {
    // A baseline scan carries the whole block whatever its header says, and
    // encoders do write nonsense here. Overriding rather than validating is
    // what libjpeg does, for the same reason.
    params.spectral_start = 0;
    params.spectral_end = 63;
    params.approximation_high = 0;
    params.approximation_low = 0;
  }
  if (params.spectral_start > 63 || params.spectral_end > 63 ||
      params.spectral_start > params.spectral_end || params.approximation_low > 13 ||
      params.approximation_high > 13) {
    return "bad spectral selection";
  }
  if (frame_.progressive && params.spectral_start == 0 && params.spectral_end != 0) {
    return "a progressive DC scan may not carry AC coefficients";
  }
  if (params.spectral_start != 0 && count != 1) {
    return "a progressive AC scan covers exactly one component";
  }
  // Only the tables the scan will actually read have to exist. A progressive AC
  // scan never touches a DC table, and files in the wild rely on that.
  for (int i = 0; i < count; ++i) {
    const JpegScanComponent& component = scan[static_cast<std::size_t>(i)];
    const bool needs_dc = params.spectral_start == 0 && params.approximation_high == 0;
    const bool needs_ac =
        params.spectral_end > 0 && !(frame_.progressive && params.spectral_start == 0);
    if ((needs_dc && !component.dc->defined) || (needs_ac && !component.ac->defined)) {
      return "scan names a Huffman table that was never defined";
    }
  }

  params.interleaved = count > 1;
  if (params.interleaved) {
    params.rows = frame_.mcus_per_column;
    params.columns = frame_.mcus_per_line;
  } else {
    // A non-interleaved scan walks the component's own block grid, which is
    // smaller than its slice of the MCU grid whenever the image does not fill
    // its last MCU. Using the MCU grid here is the classic progressive bug:
    // every row after the first is shifted.
    const JpegComponent& component = *scan[0].component;
    const int width = (frame_.width * component.h + frame_.max_h - 1) / frame_.max_h;
    const int height = (frame_.height * component.v + frame_.max_v - 1) / frame_.max_v;
    params.columns = (width + 7) / 8;
    params.rows = (height + 7) / 8;
  }

  position_ = DecodeJpegScan(
      bytes_, end, std::span<JpegScanComponent>(scan.data(), static_cast<std::size_t>(count)),
      params);
  saw_scan_ = true;
  return nullptr;
}

const char* Decoder::ReadSegments() {
  if (bytes_.size() < 4) {
    return "too short to be a JPEG";
  }
  position_ = 2;  // past SOI, which LooksLikeJpeg already checked
  while (true) {
    // Any number of fill bytes may precede a marker.
    while (position_ < bytes_.size() && Byte(position_) != 0xFF) {
      ++position_;
    }
    while (position_ < bytes_.size() && Byte(position_) == 0xFF) {
      ++position_;
    }
    if (position_ >= bytes_.size()) {
      break;
    }
    const std::uint8_t marker = Byte(position_++);
    if (marker == kEoi) {
      break;
    }
    if (marker == kSoi || (marker >= kRst0 && marker <= kRst7) || marker == 0x01) {
      continue;  // standalone markers, no length
    }
    if (position_ + 2 > bytes_.size()) {
      break;
    }
    const std::size_t length = static_cast<std::size_t>(ReadU16(position_));
    if (length < 2 || position_ + length > bytes_.size()) {
      return "segment length runs past the end of the file";
    }
    const std::size_t start = position_ + 2;
    const std::size_t end = position_ + length;
    position_ = end;

    const char* error = nullptr;
    switch (marker) {
      case kDqt:
        error = ReadQuantizationTables(start, end);
        break;
      case kDht:
        error = ReadHuffmanTables(start, end);
        break;
      case kDri:
        if (end - start < 2) {
          return "truncated restart interval";
        }
        restart_interval_ = ReadU16(start);
        break;
      case kSof0:
      case kSof1:
        error = ReadFrameHeader(start, end, false);
        break;
      case kSof2:
        error = ReadFrameHeader(start, end, true);
        break;
      case kSos:
        error = ReadScan(start, end);
        break;
      case kApp14:
        // Adobe's marker. Its transform byte is the only way to know that a
        // three-component JPEG holds RGB rather than YCbCr, and a decoder that
        // guesses produces a picture with the colours swapped.
        if (end - start >= 12 && std::memcmp(bytes_.data() + start, "Adobe", 5) == 0) {
          adobe_rgb_ = Byte(start + 11) == 0;
        }
        break;
      default:
        if (marker >= 0xC3 && marker <= 0xCF && marker != kDht && marker != 0xC8) {
          return "arithmetic, lossless and hierarchical JPEG are not supported";
        }
        break;
    }
    if (error != nullptr) {
      return error;
    }
  }
  return nullptr;
}

const char* Decoder::Run(Image& out) {
  if (const char* error = ReadSegments(); error != nullptr) {
    return error;
  }
  if (!saw_frame_) {
    return "no frame header";
  }
  if (!saw_scan_) {
    return "no scan";
  }
  for (const JpegComponent& component : frame_.components) {
    if (!quantization_defined_[static_cast<std::size_t>(component.quant_table)]) {
      return "component names a quantisation table that was never defined";
    }
  }

  frame_.samples.resize(frame_.components.size());
  for (std::size_t i = 0; i < frame_.components.size(); ++i) {
    JpegComponent& component = frame_.components[i];
    JpegPlane plane;
    plane.blocks_per_line = component.blocks_per_line;
    plane.blocks_per_column = component.blocks_per_column;
    plane.coefficients = std::span<const std::int16_t>(component.coefficients);
    plane.quantization = &quantization_[static_cast<std::size_t>(component.quant_table)];
    frame_.samples[i] = RenderJpegPlane(plane);
    // The coefficients are the larger of the two representations and nothing
    // reads them again. Holding both at once is what makes a progressive decode
    // twice the peak memory of a baseline one, so it does not.
    component.coefficients.clear();
    component.coefficients.shrink_to_fit();
    if (frame_.samples[i].empty()) {
      return "component plane failed to render";
    }
  }

  std::array<JpegSamplePlane, 3> planes{};
  for (std::size_t i = 0; i < frame_.components.size(); ++i) {
    const JpegComponent& component = frame_.components[i];
    planes[i].samples = std::span<const std::uint8_t>(frame_.samples[i]);
    planes[i].stride = static_cast<std::size_t>(component.blocks_per_line) * 8u;
    // The component's own size, not the padded plane's: the rows and columns
    // past this are whatever the encoder chose to pad the last block with, and
    // an upsampling filter that reached them would blend them into the edge of
    // the picture.
    planes[i].width = (frame_.width * component.h + frame_.max_h - 1) / frame_.max_h;
    planes[i].height = (frame_.height * component.v + frame_.max_v - 1) / frame_.max_v;
    planes[i].h = component.h;
    planes[i].v = component.v;
  }

  std::vector<std::uint32_t> pixels;
  // Three components are YCbCr unless something says otherwise, and the two
  // things that say otherwise are Adobe's transform byte and component ids that
  // spell RGB. Both are what libjpeg checks, and a decoder that guesses draws a
  // picture with the colour channels rotated.
  const bool rgb = frame_.components.size() == 3 &&
                   (adobe_rgb_ || (frame_.components[0].id == 'R' &&
                                   frame_.components[1].id == 'G' &&
                                   frame_.components[2].id == 'B'));
  if (!ComposeJpegPixels(planes, static_cast<int>(frame_.components.size()), frame_.width,
                         frame_.height, frame_.max_h, frame_.max_v, rgb, pixels)) {
    return "output allocation failed";
  }
  if (!out.Adopt(frame_.width, frame_.height, std::move(pixels))) {
    return "decoded image failed its own size check";
  }
  AddPerformanceCounter(PerfCounterId::GfxJpegPixelsDecoded,
                        static_cast<std::uint64_t>(frame_.width) *
                            static_cast<std::uint64_t>(frame_.height));
  return nullptr;
}

}  // namespace

bool LooksLikeJpeg(std::span<const std::byte> bytes) {
  // SOI followed by the start of a second marker. A two-byte signature alone
  // matches too much, and the third byte being 0xFF is what every other JPEG
  // sniffer in a browser checks.
  return bytes.size() >= 3 && static_cast<std::uint8_t>(bytes[0]) == 0xFF &&
         static_cast<std::uint8_t>(bytes[1]) == kSoi &&
         static_cast<std::uint8_t>(bytes[2]) == 0xFF;
}

JpegDecodeResult DecodeJpeg(std::span<const std::byte> bytes) {
  AddPerformanceCounter(PerfCounterId::GfxJpegDecodes);
  JpegDecodeResult result;
  if (!LooksLikeJpeg(bytes)) {
    result.error = "not a JPEG";
    AddPerformanceCounter(PerfCounterId::GfxJpegDecodeFailures);
    return result;
  }
  Decoder decoder(bytes);
  result.error = decoder.Run(result.image);
  if (result.error == nullptr && !result.image.IsValid()) {
    result.error = "decode produced no pixels";
  }
  if (result.error != nullptr) {
    AddPerformanceCounter(PerfCounterId::GfxJpegDecodeFailures);
    result.image = Image{};
  }
  return result;
}

}  // namespace microbrowser::gfx
