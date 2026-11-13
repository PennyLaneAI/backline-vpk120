#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Xanadu Quantum Technologies Inc.
#
# hw/sim/run.sh [lint|sim|all]   (default: all)
#
#   lint  Verilator lint of backline_hsb_top
#   sim   tb_hwhs_engine and tb_hwhs_hsb
#
# Needs Verilator >= 5.048.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
OUT="${OUT:-$REPO/build/sim}"
mkdir -p "$OUT"

VER="$(verilator --version | awk '{print $2}')"
if [ "$(printf '%s\n' 5.048 "$VER" | sort -V | head -1)" != 5.048 ]; then
    echo "run.sh needs Verilator 5.048 or newer, found $VER" >&2
    exit 1
fi

HSB="${HSB:-$REPO/third_party/hsb/nv_hsb_ip}"   # make passes the copy with patches/ applied
{ ls "$HSB"/*/*pkg.sv; ls "$HSB"/*/*.sv | grep -v 'pkg\.sv$'; } > "$OUT/hsb.f"
RTL=("$REPO"/hw/rtl/hwhs/*.v)
WARN=(-Wno-fatal -Wno-lint -Wno-style -Wno-WIDTH -Wno-UNOPTFLAT -Wno-TIMESCALEMOD)

what="${1:-all}"

if [ "$what" = lint ] || [ "$what" = all ]; then
    echo "== lint backline_hsb_top"
    verilator --lint-only -Wall -Wno-fatal -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL -Wno-UNUSEDPARAM \
        -Wno-UNDRIVEN -Wno-PINCONNECTEMPTY -Wno-TIMESCALEMOD -Wno-PROCASSINIT \
        --top-module backline_hsb_top +incdir+"$REPO/hw/rtl/hsb_cfg" -f "$OUT/hsb.f" \
        "$HERE/xpm_models.sv" "$HERE/lint_stubs.sv" "${RTL[@]}" "$REPO/hw/rtl/top/backline_hsb_top.sv" \
        2>&1 | grep -E '^%(Error|Warning)' | grep -v '/third_party/' || true
fi

if [ "$what" = sim ] || [ "$what" = all ]; then
    echo "== build tb_hwhs_engine"
    verilator --binary --timing --assert -j 0 "${WARN[@]}" --top-module tb_hwhs_engine -Mdir "$OUT/obj_engine" \
        "$REPO/hw/rtl/hwhs/hwhs_engine.v" "$HERE/hwhs_engine_sva.sv" "$HERE/tb_hwhs_engine.sv" \
        -o tb_hwhs_engine > "$OUT/build_engine.log" 2>&1 || { tail -40 "$OUT/build_engine.log"; exit 1; }
    echo "== run tb_hwhs_engine"
    "$OUT/obj_engine/tb_hwhs_engine"

    echo "== build tb_hwhs_hsb"
    # -fno-expand: avoids a Verilator 5.052 crash on HSB's rrarb
    verilator --binary --timing --assert -j 0 -fno-expand "${WARN[@]}" --top-module tb_hwhs_hsb -Mdir "$OUT/obj_tb" \
        +incdir+"$REPO/hw/rtl/hsb_cfg" -f "$OUT/hsb.f" "$HERE/xpm_models.sv" "${RTL[@]}" \
        "$HERE/hwhs_engine_sva.sv" "$HERE/tb_hwhs_hsb.sv" -o tb_hwhs_hsb > "$OUT/build.log" 2>&1 || { tail -40 "$OUT/build.log"; exit 1; }
    echo "== run tb_hwhs_hsb"
    "$OUT/obj_tb/tb_hwhs_hsb"
    echo "== run tb_hwhs_hsb +plain_write"
    "$OUT/obj_tb/tb_hwhs_hsb" +plain_write
fi
