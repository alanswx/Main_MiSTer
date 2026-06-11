#!/usr/bin/env bash
#
# iigs_regression.sh — end-to-end regression for the IIgs disk-format codec.
#
# Two layers:
#   1. Native round-trip tests (support/a2/tests) — pure codec, no sim.
#   2. Boot tests: convert real .po/.dsk fixtures to WOZ with iigs_convert,
#      boot each in the Verilator sim, screenshot, and either diff against a
#      saved reference (regression) or classify by screenshot size (baseline).
#
# Like the sim's regression.sh, but it exercises OUR converters: a disk that
# boots proves the generated WOZ is one the core can actually read.
#
# Usage:
#   ./iigs_regression.sh                 # run everything
#   ./iigs_regression.sh --bless         # save current screenshots as references
#   ./iigs_regression.sh --frames 800    # frames per boot (default 700)
#
# Env overrides:
#   SIM_DIR    path to iigs_simulation/vsim (has obj_dir/Vemu)
#   FIXTURES   path to disk-image fixtures
#   TIMEOUT    per-boot wall-clock seconds (default 240)

set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

FRAMES=700
TIMEOUT="${TIMEOUT:-240}"
BLESS=0
SIM_DIR="${SIM_DIR:-$HERE/../../../../threefivefixes/iigs_simulation/vsim}"
FIXTURES="${FIXTURES:-$HERE/../../../artifacts_for_iigs}"
REFDIR="$HERE/regression_images"
WORK="$(mktemp -d -t iigs_reg_XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

while [[ $# -gt 0 ]]; do
  case "$1" in
    --bless)  BLESS=1; shift ;;
    --frames) FRAMES="$2"; shift 2 ;;
    --help|-h) sed -n '3,26p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

VEMU="$SIM_DIR/obj_dir/Vemu"
CONVERT="$HERE/iigs_convert"
TO=$(command -v gtimeout || command -v timeout || true)
PASS=0; FAIL=0; SKIP=0

say() { printf '%s\n' "$*"; }
hr()  { printf -- '----------------------------------------\n'; }

# Test cases: "label|convert-cmd|fixture-relpath"
CASES=(
  "system-disk-3.5|po2woz|System.Disk.po"
  "tour-iigs-2.0-3.5|po2woz|Tour of the Apple IIGS 2.0.po"
  "dos33-5.25|dsk2woz|Apple DOS 3.3 January 1983.dsk"
)

# ---------------------------------------------------------------------------
# Layer 1: native codec round-trip tests
# ---------------------------------------------------------------------------
say "== Layer 1: native codec round-trip tests =="
if make -C "$HERE/../tests" run FIXTURES="$FIXTURES" >"$WORK/native.log" 2>&1; then
  tail -1 "$WORK/native.log"
  say "  PASS: native round-trip suite"
  PASS=$((PASS+1))
else
  cat "$WORK/native.log"
  say "  FAIL: native round-trip suite"
  FAIL=$((FAIL+1))
fi
hr

# ---------------------------------------------------------------------------
# Layer 2: boot tests
# ---------------------------------------------------------------------------
say "== Layer 2: convert + boot in sim =="

if [[ ! -x "$VEMU" ]]; then
  say "  SKIP: sim not built ($VEMU). Build with: (cd $SIM_DIR && make)"
  SKIP=$((SKIP+1))
else
  make -C "$HERE" >/dev/null 2>&1 || { say "  FAIL: building iigs_convert"; exit 1; }
  mkdir -p "$REFDIR"

  for entry in "${CASES[@]}"; do
    IFS='|' read -r label cmd rel <<< "$entry"
    src="$FIXTURES/$rel"
    if [[ ! -f "$src" ]]; then
      say "  SKIP: $label (missing fixture: $rel)"; SKIP=$((SKIP+1)); continue
    fi

    woz="$WORK/$label.woz"
    if ! "$CONVERT" "$cmd" "$src" "$woz" >/dev/null 2>&1; then
      say "  FAIL: $label (conversion failed)"; FAIL=$((FAIL+1)); continue
    fi

    shot="$WORK/$label.png"
    # boot in sim (run from SIM_DIR so it finds its ROMs)
    ( cd "$SIM_DIR" && ${TO:+$TO $TIMEOUT} ./obj_dir/Vemu --quiet --no-cpu-log \
        --woz "$woz" --stop-at-frame "$FRAMES" --screenshot "$FRAMES" \
        --screenshot-name "$shot" >/dev/null 2>&1 )
    rc=$?

    if [[ ! -f "$shot" ]]; then
      say "  FAIL: $label (no screenshot, rc=$rc — crash/timeout)"; FAIL=$((FAIL+1)); continue
    fi
    sz=$(stat -f%z "$shot" 2>/dev/null || stat -c%s "$shot")

    ref="$REFDIR/$label.png"
    if [[ "$BLESS" -eq 1 ]]; then
      cp "$shot" "$ref"; say "  BLESS: $label (saved reference, ${sz} bytes)"; PASS=$((PASS+1)); continue
    fi

    if [[ -f "$ref" ]]; then
      if cmp -s "$shot" "$ref"; then
        say "  PASS: $label (matches reference, ${sz} bytes)"; PASS=$((PASS+1))
      else
        cp "$shot" "$WORK/$label.actual.png"
        say "  FAIL: $label (screenshot differs from reference; actual in $WORK/$label.actual.png)"; FAIL=$((FAIL+1))
      fi
    else
      # No reference yet: classify by size (mirrors test_woz_batch heuristic).
      if   [[ "$sz" -lt 5600 ]]; then status="BLANK";   ok=0
      elif [[ "$sz" -lt 6000 ]]; then status="TEXT";    ok=1
      else                            status="BOOTED";  ok=1; fi
      if [[ "$ok" -eq 1 ]]; then
        cp "$shot" "$ref"
        say "  PASS: $label ($status, ${sz} bytes — saved as new reference)"; PASS=$((PASS+1))
      else
        say "  FAIL: $label ($status, ${sz} bytes — disk did not boot)"; FAIL=$((FAIL+1))
      fi
    fi
  done
fi

hr
say "RESULT: PASS $PASS   FAIL $FAIL   SKIP $SKIP"
[[ "$FAIL" -eq 0 ]]
