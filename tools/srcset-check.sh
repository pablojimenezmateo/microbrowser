#!/usr/bin/env bash
# Session 6's check: a high-density snapshot picks the 2x candidate.
#
# The page is a data: URL and both candidates are data: URLs inside it, so this
# renders the real seam -- ipc into engine::Engine, a display list back -- with
# no network, no display and no fixture files. What it proves is which of two
# images the selection algorithm chose, and it proves it twice over: the 1x
# candidate is a 20x20 red square and the 2x is a 40x40 blue one, so the size in
# the display list and the colour in the pixels are two independent witnesses.
#
#   tools/srcset-check.sh        # prints each display list, then PASS or FAIL
set -euo pipefail

SNAPSHOT=${SNAPSHOT:-./build/microbrowser/microbrowser_snapshot}
RED_20=iVBORw0KGgoAAAANSUhEUgAAABQAAAAUCAIAAAAC64paAAAAGklEQVR42mO4IyJCNmIY1TyqeVTzqOaB1QwAeouWUCpFMc8AAAAASUVORK5CYII=
BLUE_40=iVBORw0KGgoAAAANSUhEUgAAACgAAAAoCAIAAAADnC86AAAAL0lEQVR42u3NMQ0AAAgDsImYf2WIwQQJT5P+TTsvIhaLxWKxWCwWi8VisVgsFt9ZCOhZW6qzaOMAAAAASUVORK5CYII=

page() {
  # One <img> with both candidates, and a <picture> whose <source> names a
  # format we cannot decode -- so the right answer for it is the <img> fallback
  # rather than the source, which is the whole point of `type`.
  printf 'data:text/html,<body style="margin:0">'
  printf '<img srcset="data:image/png;base64,%s 1x, data:image/png;base64,%s 2x">' "$RED_20" "$BLUE_40"
  printf '<picture><source type="image/webp" srcset="data:image/png;base64,%s 1x">' "$BLUE_40"
  printf '<img src="data:image/png;base64,%s"></picture>' "$RED_20"
}

for dpr in 1 2; do
  echo "=== device pixel ratio ${dpr} ==="
  "$SNAPSHOT" "$(page)" -o "/tmp/srcset-${dpr}x.ppm" -dpr "$dpr" -v 2>&1 |
    grep -E '^\s+\[' | sed 's/^/  /'
done

# The pixel at (10,10) is inside the first image at either density; the one at
# (50,30) is inside the second image only when it was drawn at 20x20 after a
# 40-pixel-wide neighbour, which is what the fallback path produces at 2x.
python3 - <<'PY'
import sys

def pixel(path, x, y):
    data = open(path, 'rb').read()
    start = data.index(b'255\n') + 4
    offset = start + (y * 1280 + x) * 3
    return tuple(data[offset:offset + 3])

RED, BLUE = (220, 20, 20), (20, 20, 220)
checks = [
    ('at 1x the img picks the 1x candidate', pixel('/tmp/srcset-1x.ppm', 10, 10), RED),
    ('at 2x the img picks the 2x candidate', pixel('/tmp/srcset-2x.ppm', 10, 10), BLUE),
    ('at 1x the picture falls back to the img', pixel('/tmp/srcset-1x.ppm', 30, 10), RED),
    ('at 2x the picture still declines the webp source', pixel('/tmp/srcset-2x.ppm', 50, 30), RED),
]
failed = False
for name, actual, expected in checks:
    ok = actual == expected
    failed = failed or not ok
    print(f"  {'ok  ' if ok else 'FAIL'} {name}: {actual}")
print('FAIL' if failed else 'PASS')
sys.exit(1 if failed else 0)
PY
