#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Xanadu Quantum Technologies Inc.
#
# Unpack the files the build needs from AMD's VPK120 ERNIC MRMAC reference design.
#
#   scripts/unpack-amd.sh <AMD zip or unpacked folder> [--bsp] [out_dir]   (out_dir defaults to build/amd)
set -euo pipefail

SRC=""
OUT=""
WITH_BSP=0
for a in "$@"; do
    case "$a" in
        --bsp) WITH_BSP=1 ;;
        *) if [ -z "$SRC" ]; then SRC="$a"; else OUT="$a"; fi ;;
    esac
done
[ -n "$SRC" ] || { echo "usage: $0 <AMD zip or unpacked folder> [--bsp] [out_dir]" >&2; exit 1; }

REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$REPO/build/amd}"
BASE="reference design VPK120_mrmac_rev_1.3/reference_designs_VPK120_mrmac/designs/2024.2/vpk120m_ernic_ref_design"

# Board device tree, from the ERNIC software package
ERNIC_SW_ZIP="ERNIC-2024_2_VPK120_RC6.zip"
BOARD_DTSI="ERNIC-2024_2_VPK120_RC6/code/meta-user/recipes-bsp/vpk120_mrmac/device-tree/files/versal-vpk120-revB.dtsi"

# SHA-256 of the files the build reads (rev 1.3)
EXPECTED_SHA256="\
6ff1a33c83a62ccd39a615f274b0a80b5c1eec6c1418795da385b004f6cdfd32  hw/vpk120m_ernic_ref_design.tcl
0ead91f374e7f2432ed0f9807fa7e74f82787bb9d898ba38874037df9b3d3e45  hw/hdl/mrmac_0_exdes_ernic.sv
461dedea110f26244f20a0dec22d3993c7ed37f7cfdd45312716c8cbeaee903c  hw/hdl/mrmac_0_axis_stream_mux.sv
c52c0b93b48858ad4654b1f91321e7fe5f2cedd25591dbd9ba17f7f07ef3b914  hw/clk_wizard_0.xci
605b6841123b8b877861bdc8c447080834e469d12e6b8d66ee7b985fa6f3fe73  hw/v350_ddr4_single_lp0.xdc
8f0fdc4171ef3bdbe93117a7e5c347edf6e1456c1653c4aeef69fdb0ea482b80  hw/default.xdc
5423de75ca463fde88fb0b4ef57d70b62350ecca0816226e4e7e0869c230de77  sw/versal-vpk120-revB.dtsi"

TMP=""
cleanup() { if [ -n "$TMP" ]; then rm -rf "$TMP"; fi; }
trap cleanup EXIT

if [ -f "$SRC" ]; then
    TMP="$(mktemp -d)"
    members=("$BASE/hw/hw/*" "$BASE/sw/$ERNIC_SW_ZIP")
    [ "$WITH_BSP" = 1 ] && members+=("$BASE/sw/xilinx-vpk120-xsct-v2024.2-final.bsp")
    unzip -q "$SRC" "${members[@]}" -d "$TMP"
    ROOT="$TMP/$BASE"
elif [ -d "$SRC/$BASE" ]; then
    ROOT="$SRC/$BASE"
elif [ -d "$SRC/reference_designs_VPK120_mrmac" ]; then
    ROOT="$SRC/reference_designs_VPK120_mrmac/designs/2024.2/vpk120m_ernic_ref_design"
else
    ROOT="$SRC"
fi
[ -f "$ROOT/hw/hw/vpk120m_ernic_ref_design.tcl" ] || {
    echo "ERROR: $SRC is not AMD's VPK120 ERNIC MRMAC reference design (no hw/hw/vpk120m_ernic_ref_design.tcl)" >&2
    exit 1
}

[ -f "$ROOT/sw/$ERNIC_SW_ZIP" ] || {
    echo "ERROR: no sw/$ERNIC_SW_ZIP in $SRC (the board device tree comes from it)" >&2
    exit 1
}

rm -rf "$OUT"
mkdir -p "$OUT/hw" "$OUT/sw"
cp -R "$ROOT/hw/hw/." "$OUT/hw/"
unzip -p "$ROOT/sw/$ERNIC_SW_ZIP" "$BOARD_DTSI" > "$OUT/sw/versal-vpk120-revB.dtsi"

if [ "$WITH_BSP" = 1 ]; then
    cp "$ROOT/sw/xilinx-vpk120-xsct-v2024.2-final.bsp" "$OUT/sw/"
fi

if ! (cd "$OUT" && echo "$EXPECTED_SHA256" | shasum -a 256 -c --quiet -); then
    echo "WARNING: these AMD files differ from rev 1.3 (ERNIC-2024_2_VPK120_RC6); the build was written against that release." >&2
fi

echo "AMD reference design unpacked to $OUT"
if [ "$WITH_BSP" = 1 ]; then echo "BSP: $OUT/sw/xilinx-vpk120-xsct-v2024.2-final.bsp"; fi
exit 0
