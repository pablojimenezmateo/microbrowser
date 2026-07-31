#!/usr/bin/env bash
#
# Prints how much headroom each module and class has left against its
# MODULE.deps budget, sorted by how close it is to the limit.
#
# The architecture lint tells you when a budget has already been blown. This
# tells you what is about to blow, which is the more useful moment to look —
# a class at 95% of its budget is a design decision waiting to be made, not an
# emergency.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

python3 - "$@" <<'PY'
import pathlib
import re
import sys

repo = pathlib.Path.cwd()
src = repo / "src"

rows = []

for manifest_path in sorted(src.glob("*/MODULE.deps")):
    module = manifest_path.parent.name
    max_tu_lines = 0
    budgets = {}

    for line in manifest_path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#") or ":" not in line:
            continue
        field, _, value = line.partition(":")
        field, value = field.strip(), value.strip()
        if field == "max_tu_lines":
            max_tu_lines = int(value)
        elif field == "budget":
            parts = value.split()
            name = parts[0]
            budgets[name] = {
                k: int(v)
                for k, v in (p.split("=", 1) for p in parts[1:])
            }

    # Translation unit sizes.
    for source in sorted(manifest_path.parent.glob("*.[hc]*")):
        if source.suffix not in (".h", ".cpp"):
            continue
        lines = len(source.read_text().splitlines())
        if max_tu_lines:
            rows.append((lines / max_tu_lines, f"{module}/{source.name}",
                         "tu_lines", lines, max_tu_lines))

    # Class header sizes, measured the same way the lint does: opening line
    # through the matching closing brace.
    for header in sorted(manifest_path.parent.glob("*.h")):
        text = header.read_text()
        for match in re.finditer(r"^(?:class|struct)\s+(\w+)[^;{]*\{", text, re.M):
            name = match.group(1)
            budget = budgets.get(name)
            if not budget or "header_lines" not in budget:
                continue
            depth = 0
            end = match.end() - 1
            for i in range(match.end() - 1, len(text)):
                if text[i] == "{":
                    depth += 1
                elif text[i] == "}":
                    depth -= 1
                    if depth == 0:
                        end = i
                        break
            lines = text.count("\n", match.start(), end) + 1
            limit = budget["header_lines"]
            rows.append((lines / limit, f"{module}::{name}", "header_lines", lines, limit))

rows.sort(reverse=True)

print(f"{'USED':>6}  {'WHAT':<40} {'METRIC':<14} {'NOW':>6} {'LIMIT':>6}")
print("-" * 78)
for fraction, what, metric, now, limit in rows:
    flag = "  <-- over" if now > limit else ("  <-- tight" if fraction >= 0.85 else "")
    print(f"{fraction * 100:5.0f}%  {what:<40} {metric:<14} {now:>6} {limit:>6}{flag}")
PY
