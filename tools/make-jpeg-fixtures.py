#!/usr/bin/env python3
"""Regenerates tests/support/JpegFixtures.cpp.

The JPEG decoder's fixtures are produced by an encoder this project did not
write, and decoded by a decoder this project did not write, because that is the
only part of a decoder test that cannot be satisfied by agreeing with itself. A
synthetic builder in the shape of tests/support/SyntheticPng.h would prove that
our encoder and our decoder are inverses, which is a weaker statement than the
one that matters.

So: Pillow (libjpeg-turbo) encodes each fixture and decodes it back, and both
the file and the pixels it should produce are checked in. The decoder then has
to agree with libjpeg to within a tolerance the test states, which is the right
comparison for an IDCT — two conforming implementations differ by a fraction of
a level, and T.83 says so.

Run from the repository root, with Pillow installed:

    python3 tools/make-jpeg-fixtures.py
"""

import io
import sys

from PIL import Image

OUT = "tests/support/JpegFixtures.cpp"


def gradient(width, height, mode):
    """A picture with a lot of high-frequency content and a wrapping ramp.

    Good for the transform and for colour conversion, which see every pixel
    independently. Wrong for anything subsampled: the modulo wraps produce
    241-level cliffs, and at a cliff the difference between two upsampling
    filters is the cliff rather than a rounding error.
    """
    image = Image.new(mode, (width, height))
    for y in range(height):
        for x in range(width):
            if mode == "L":
                image.putpixel((x, y), (x * 251 + y * 37) % 256)
            else:
                image.putpixel(
                    (x, y),
                    (
                        (x * 17 + 8) % 256,
                        (y * 29 + 60) % 256,
                        (x * 5 + y * 41 + 20) % 256,
                    ),
                )
    return image


def triangle(value, period):
    position = value % period
    return abs(position - period / 2.0) * 2.0 / period


def continuous(width, height):
    """A picture with AC energy in every block and no discontinuity anywhere.

    This is what the subsampled fixtures use. A chroma plane at half resolution
    cannot represent a cliff, so a fixture containing one measures which
    upsampling filter the decoder chose rather than whether it decoded; a
    continuous picture measures alignment, which is the thing that actually
    goes wrong.
    """
    image = Image.new("RGB", (width, height))
    for y in range(height):
        for x in range(width):
            red = 30 + 100 * x / max(1, width - 1) + 60 * triangle(x, 8)
            green = 30 + 100 * y / max(1, height - 1) + 60 * triangle(y, 6)
            blue = 30 + 80 * (x + y) / max(1, width + height - 2) + 60 * triangle(x + y, 10)
            image.putpixel((x, y), (int(red), int(green), int(blue)))
    return image


def encode(image, **options):
    buffer = io.BytesIO()
    image.save(buffer, "JPEG", **options)
    return buffer.getvalue()


def decoded_rgb(data):
    image = Image.open(io.BytesIO(data))
    image = image.convert("RGB")
    return image.tobytes()


def hex_array(name, data):
    lines = [f"constexpr std::uint8_t {name}[] = {{"]
    for start in range(0, len(data), 12):
        chunk = data[start : start + 12]
        lines.append("    " + " ".join(f"0x{byte:02X}," for byte in chunk))
    lines.append("};")
    return "\n".join(lines)


def main():
    fixtures = []

    # Greyscale: one component, so no upsampling and no colour conversion. If
    # this one is wrong, the Huffman decoder or the IDCT is wrong and nothing
    # else can be trusted.
    fixtures.append(("Gray", gradient(16, 16, "L"), dict(quality=95)))

    # 4:4:4, three components, no subsampling. Isolates colour conversion from
    # upsampling the same way the one above isolates the transform.
    fixtures.append(("Yuv444", gradient(16, 16, "RGB"), dict(quality=95, subsampling=0)))

    # 4:2:0 at a size that is not a multiple of the MCU, so the last MCU is
    # padding the decoder has to throw away, and chroma is upsampled 2x in both
    # axes. This is what an ordinary photograph on the web looks like.
    fixtures.append(("Yuv420", continuous(17, 9), dict(quality=90, subsampling=2)))

    # Progressive: ten scans, spectral selection and successive approximation,
    # which is a completely different decoder path from the three above.
    fixtures.append(
        ("Progressive", continuous(24, 16), dict(quality=90, progressive=True))
    )

    # A restart marker after every MCU: the entropy stream is byte-aligned and
    # the DC predictors reset, five times, mid-scan. The size matters — an
    # interval that divides the MCU count exactly produces no markers at all,
    # which is a fixture that tests nothing and looks like one that does.
    fixtures.append(
        ("Restarts", continuous(48, 32), dict(quality=90, restart_marker_blocks=1))
    )

    parts = []
    entries = []
    for name, image, options in fixtures:
        data = encode(image, **options)
        pixels = decoded_rgb(data)
        assert len(pixels) == image.width * image.height * 3
        parts.append(hex_array(f"k{name}Jpeg", data))
        parts.append(hex_array(f"k{name}Rgb", pixels))
        entries.append((name, image.width, image.height))

    with open(OUT, "w", encoding="utf-8") as out:
        out.write('#include "support/JpegFixtures.h"\n\n')
        out.write("#include <cstdint>\n\n")
        out.write("// GENERATED by tools/make-jpeg-fixtures.py. Do not edit by hand.\n")
        out.write("//\n")
        out.write("// Each fixture is a JPEG produced by libjpeg-turbo through Pillow, and the\n")
        out.write("// RGB libjpeg-turbo decodes it back to. The reasoning is in the script.\n\n")
        out.write("namespace microbrowser::tests {\n\nnamespace {\n\n")
        out.write("\n\n".join(parts))
        out.write("\n\n}  // namespace\n\n")
        for name, width, height in entries:
            out.write(
                f"JpegFixture {name}Fixture() {{\n"
                f"  JpegFixture fixture;\n"
                f"  fixture.width = {width};\n"
                f"  fixture.height = {height};\n"
                f"  fixture.bytes.reserve(sizeof(k{name}Jpeg));\n"
                f"  for (const std::uint8_t value : k{name}Jpeg) {{\n"
                f"    fixture.bytes.push_back(static_cast<std::byte>(value));\n"
                f"  }}\n"
                f"  fixture.rgb.assign(std::begin(k{name}Rgb), std::end(k{name}Rgb));\n"
                f"  return fixture;\n"
                f"}}\n\n"
            )
        out.write("}  // namespace microbrowser::tests\n")

    sizes = ", ".join(f"{name} {width}x{height}" for name, width, height in entries)
    print(f"wrote {OUT}: {sizes}", file=sys.stderr)


if __name__ == "__main__":
    main()
