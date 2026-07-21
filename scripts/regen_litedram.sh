#!/usr/bin/env bash
# Regenerate the committed LiteDRAM core under
#   <dram_ctrl>/generated/
# from the pinned third_party/{migen,litex,litedram} submodules.
#
# Everyday `make build USE_DRAM_CTRL=1` consumes the committed artifacts and
# needs no Python; run this only when bumping the submodules or editing
# litedram_dram_ctrl.yml / litedram_gen.py (this script's sibling).
set -euo pipefail

# This script lives at <dram_ctrl>/scripts/, so <dram_ctrl> is the script
# dir's parent; the repo root (where the third_party submodules live) comes
# from git.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DCTL="$(cd "$SCRIPT_DIR/.." && pwd)"
ROOT="$(git -C "$DCTL" rev-parse --show-toplevel)"
VENV="$ROOT/third_party/.venv"
BUILD="$DCTL/build"

if [ ! -x "$VENV/bin/python" ]; then
    echo "[regen_litedram] creating venv + installing pinned submodules"
    python3 -m venv "$VENV"
    "$VENV/bin/pip" -q install pyyaml packaging pythondata-misc-tapcfg
    "$VENV/bin/pip" -q install -e "$ROOT/third_party/migen" \
                               -e "$ROOT/third_party/litex" \
                               -e "$ROOT/third_party/litedram"
fi

rm -rf "$BUILD"
"$VENV/bin/python" "$SCRIPT_DIR/litedram_gen.py" \
    "$DCTL/litedram_dram_ctrl.yml" \
    --sim \
    --name litedram_core \
    --output-dir "$BUILD" \
    --csr-csv "$BUILD/csr.csv"

mkdir -p "$DCTL/generated"
# Strip the `timescale directive: this Verilator flow is cycle-based and
# no other module declares one (mixed timescales trip TIMESCALEMOD design-wide).
sed 's|^`timescale .*|// (timescale stripped: cycle-based Verilator flow; no other module declares one)|' \
    "$BUILD/gateware/litedram_core.v" > "$DCTL/generated/litedram_core.v"
cp "$BUILD/csr.csv"                  "$DCTL/generated/csr.csv"
rm -rf "$BUILD"

echo "[regen_litedram] done; diff vs committed:"
git -C "$ROOT" --no-pager diff --stat -- "$DCTL/generated" || true
