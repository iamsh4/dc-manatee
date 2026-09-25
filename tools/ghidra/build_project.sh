#!/bin/sh
# Build the fully annotated Ghidra project for the Manatee driver from bin/manatee_arm.bin.
#
#   tools/ghidra/build_project.sh            -> ghidra/Manatee.gpr (program "manatee_arm.bin")
#
# Steps: verify the image's SHA-256, import it as raw ARM (v4, little endian) at address 0,
# run SetupManatee.java (memory map, AICA registers, vectors) before auto-analysis, then
# ApplyManatee.java (twice) with every annotation file in tools/ghidra/annotations/ (names,
# register signatures, types from src/arm/manatee.h, comments, jump tables, locals).
# Requires Ghidra 12.x; set GHIDRA_INSTALL_DIR if it is not in the Homebrew location.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
GHIDRA=${GHIDRA_INSTALL_DIR:-/opt/homebrew/opt/ghidra/libexec}
HEADLESS="$GHIDRA/support/analyzeHeadless"
IMAGE=bin/manatee_arm.bin
EXPECTED=44f3ab80428e7d5c1f619af6c39ae038028779de232a0cb27ce4911ea15cd9c6

[ -x "$HEADLESS" ] || { echo "analyzeHeadless not found; set GHIDRA_INSTALL_DIR" >&2; exit 1; }
[ -f "$IMAGE" ] || { echo "$IMAGE missing: run tools/extract_sdrv.py first (see README)" >&2; exit 1; }
SUM=$(shasum -a 256 "$IMAGE" | cut -d' ' -f1)
[ "$SUM" = "$EXPECTED" ] || { echo "SHA-256 mismatch for $IMAGE ($SUM); expected $EXPECTED" >&2; exit 1; }
if [ -e ghidra/Manatee.gpr ]; then
  echo "ghidra/Manatee.gpr already exists; delete ghidra/ to rebuild, or use apply_live.sh to update it" >&2
  exit 1
fi

python3 tools/gen_ghidra_types.py
mkdir -p ghidra
"$HEADLESS" ghidra Manatee \
  -import "$IMAGE" -loader BinaryLoader -loader-baseAddr 0 -processor ARM:LE:32:v4 \
  -scriptPath tools/ghidra \
  -preScript SetupManatee.java \
  -postScript ApplyManatee.java "$ROOT/tools/ghidra/annotations" \
  2>&1 | grep -E 'ApplyManatee:|ERROR|REPORT: (Import|Save)' || true
# Second pass: some local-variable keys only resolve once the first pass's types/renames exist.
"$HEADLESS" ghidra Manatee -process manatee_arm.bin -noanalysis \
  -scriptPath tools/ghidra -postScript ApplyManatee.java "$ROOT/tools/ghidra/annotations" \
  2>&1 | grep -E 'ApplyManatee:|ERROR' || true
echo "Done: open ghidra/Manatee.gpr in Ghidra."
