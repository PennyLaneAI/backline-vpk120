#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Xanadu Quantum Technologies Inc.
#
# hw/hls/csim/run.sh - build and run csim_balboa: BALBOA's rocev2 HLS core as plain C++
# (third_party/balboa, unmodified) against the board's driver packing and an RC peer.
#
# Needs a C++ compiler and the HLS arbitrary-precision headers (ap_int.h): Vitis HLS's
# ($XILINX_HLS/include) when it is installed, else Xilinx's open-source copy (Apache-2.0),
# cloned into build/csim at a fixed commit. hls::stream comes from hls_stream.h here.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
OUT="${OUT:-$REPO/build/csim}"
HLS="$REPO/third_party/balboa/fpga-network-stack/hls"
CXX="${CXX:-c++}"
CC="${CC:-cc}"
APT_URL=https://github.com/Xilinx/HLS_arbitrary_Precision_Types.git
APT_REV=200a9aecaadf471592558540dc5a88256cbf880f
mkdir -p "$OUT/cfg" "$OUT/obj"

# The ap_int headers do not build against libc++, macOS's C++ library
if [ "$(uname)" = Darwin ] && "$CXX" --version 2>/dev/null | grep -q clang; then
    echo "csim: on macOS build with GCC (brew install gcc): CXX=g++-15 $0, or make csim CSIM_CXX=g++-15" >&2
    exit 1
fi

# ap_int.h
if [ -n "${XILINX_HLS:-}" ] && [ -f "$XILINX_HLS/include/ap_int.h" ]; then
    APINC="$XILINX_HLS/include"
else
    if [ ! -f "$OUT/hls_apt/include/ap_int.h" ]; then
        rm -rf "$OUT/hls_apt"
        git clone --quiet "$APT_URL" "$OUT/hls_apt"
        git -C "$OUT/hls_apt" checkout --quiet "$APT_REV"
    fi
    APINC="$OUT/hls_apt/include"
fi

# The config headers, as CMake's configure_file writes them for Coyote's defaults
cfg() { sed -e 's/${DATA_WIDTH}/64/' -e 's/${ROCE_STACK_MAX_QPS}/500/' -e 's/${PMTU_BYTES}/4096/' "$1" > "$OUT/cfg/$2"; }
cfg "$HLS/rocev2/rocev2_config.hpp.in" rocev2_config.hpp
cfg "$HLS/ipv4/ipv4_config.hpp.in" ipv4_config.hpp
cfg "$HLS/ipv6/ipv6_config.hpp.in" ipv6_config.hpp
cfg "$HLS/udp/udp_config.hpp.in" udp_config.hpp
if grep -q '\${' "$OUT"/cfg/*.hpp; then echo "csim: unfilled template variable in $OUT/cfg" >&2; exit 1; fi

# HERE first: hls_stream.h and ap_axi_sdata.h
INC=(-I"$HERE" -I"$OUT/cfg" -I"$APINC" -I"$REPO/sw/lib/hwhs" -I"$REPO/common/include")
CXXFLAGS=(-std=c++14 -O1 -w)

SRCS=(axi_utils.cpp ipv4/ipv4_utils.cpp ipv4/ipv4.cpp ipv6/ipv6.cpp udp/udp.cpp crc/crc.cpp
      ib_transport_protocol/ib_utils.cpp ib_transport_protocol/ib_transport_protocol.cpp rocev2/rocev2.cpp)
OBJS=()
for s in "${SRCS[@]}"; do
    o="$OUT/obj/$(echo "$s" | tr / _).o"
    "$CXX" "${CXXFLAGS[@]}" "${INC[@]}" -c "$HLS/$s" -o "$o"
    OBJS+=("$o")
done
"$CC" -O1 -Wall -I"$REPO/common/include" -c "$REPO/sw/lib/hwhs/hwhs.c" -o "$OUT/obj/hwhs.o"
"$CXX" "${CXXFLAGS[@]}" "${INC[@]}" -I"$HLS/rocev2" "$HERE/csim_balboa.cpp" "${OBJS[@]}" "$OUT/obj/hwhs.o" \
    -o "$OUT/csim_balboa"
"$OUT/csim_balboa"
