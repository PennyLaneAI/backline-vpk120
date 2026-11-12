#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Xanadu Quantum Technologies Inc.
#
# Synthesize BALBOA's HLS cores with Vitis HLS 2024.2 and export them as Vivado IP.
#
#   hw/scripts/build_hls.sh [-net_mhz 250] [-out build/hls] [core ...]
#
# Cores: rocev2 ip_handler mac_ip_encode arp_server_subnet (default: all four). Each runs in
# out/<core>/ from the vendored make.tcl.in, filled in the way Coyote's CMake fills it
# (configure_file), and lands in out/iprepo/<core>/, which hw/scripts/create_project.tcl adds
# to the IP catalog; out/iprepo/<core>/net_mhz records the clock it was synthesized for. The
# config headers are written the same way, with Coyote's defaults: 64-byte data path,
# 4096-byte PMTU, 500 QPs.
#
# Needs vitis_hls (Vitis HLS 2024.2) or vitis-run on PATH: source the Vitis settings64.sh.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
HLS="$REPO/third_party/balboa/fpga-network-stack/hls"
OUT="$REPO/build/hls"
NET_MHZ=250
PART=xcvp1202-vsva2785-2MP-e-S
CORES=()

while [ $# -gt 0 ]; do
    case "$1" in
        -net_mhz) NET_MHZ="$2"; shift 2 ;;
        -out)     OUT="$2"; shift 2 ;;
        *)        CORES+=("$1"); shift ;;
    esac
done
[ ${#CORES[@]} -gt 0 ] || CORES=(rocev2 ip_handler mac_ip_encode arp_server_subnet)

if command -v vitis_hls >/dev/null; then
    run_hls() { vitis_hls -f make.tcl; }
elif command -v vitis-run >/dev/null; then
    run_hls() { vitis-run --mode hls --tcl make.tcl; }
else
    echo "ERROR: no vitis_hls or vitis-run on PATH; source the Vitis 2024.2 settings64.sh" >&2
    exit 1
fi

PERIOD="$(awk -v m="$NET_MHZ" 'BEGIN { printf "%.3f", 1000.0 / m }')"
# Coyote's HLS_CLOCK_UNCERTAINTY; make.tcl.in reads it as CLOCK_UNCERTAINTY
UNCERTAINTY="27%"
mkdir -p "$OUT/iprepo"

# configure_file: replace the variables, fail on any left over
fill() {   # fill <template> <output> <source dir> <binary dir> <project>
    sed -e "s|\${DATA_WIDTH}|64|g" -e "s|\${ROCE_STACK_MAX_QPS}|500|g" -e "s|\${PMTU_BYTES}|4096|g" \
        -e "s|\${FPGA_PART}|$PART|g" -e "s|\${CLOCK_PERIOD}|$PERIOD|g" -e "s|\${CLOCK_UNCERTAINTY}|$UNCERTAINTY|g" \
        -e "s|\${CMAKE_CURRENT_SOURCE_DIR}|$3|g" -e "s|\${CMAKE_CURRENT_BINARY_DIR}|$4|g" \
        -e "s|\${IPREPO_DIR}|$OUT/iprepo|g" -e "s|\${PROJECT_NAME}|$5|g" "$1" > "$2"
    if grep -n '\${' "$2" >&2; then echo "ERROR: unfilled variable in $2" >&2; exit 1; fi
}

for core in "${CORES[@]}"; do
    src="$HLS/$core"
    bin="$OUT/$core"
    [ -f "$src/make.tcl.in" ] || { echo "ERROR: no HLS core $core" >&2; exit 1; }
    rm -rf "$bin" "$OUT/iprepo/$core"
    mkdir -p "$bin"
    case "$core" in
        rocev2)
            fill "$HLS/ipv4/ipv4_config.hpp.in" "$bin/ipv4_config.hpp" "$src" "$bin" "$core"
            fill "$HLS/ipv6/ipv6_config.hpp.in" "$bin/ipv6_config.hpp" "$src" "$bin" "$core"
            fill "$HLS/udp/udp_config.hpp.in" "$bin/udp_config.hpp" "$src" "$bin" "$core"
            fill "$src/rocev2_config.hpp.in" "$bin/rocev2_config.hpp" "$src" "$bin" "$core" ;;
        *)
            fill "$src/${core}_config.hpp.in" "$bin/${core}_config.hpp" "$src" "$bin" "$core" ;;
    esac
    fill "$src/make.tcl.in" "$bin/make.tcl" "$src" "$bin" "$core"
    echo "== $core ($PART, ${PERIOD} ns, uncertainty $UNCERTAINTY)"
    (cd "$bin" && run_hls > "$bin/hls.log" 2>&1) || { tail -40 "$bin/hls.log"; echo "ERROR: $core failed, see $bin/hls.log" >&2; exit 1; }
    [ -f "$OUT/iprepo/$core/component.xml" ] || { echo "ERROR: $core exported no IP (see $bin/hls.log)" >&2; exit 1; }
    echo "$NET_MHZ" > "$OUT/iprepo/$core/net_mhz"
    grep -m1 "|ap_clk" "$bin/${core}_prj/solution1/syn/report/${core}_top_csynth.rpt" 2>/dev/null || true
done
echo "HLS IP in $OUT/iprepo"
