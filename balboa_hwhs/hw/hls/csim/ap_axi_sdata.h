// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Xanadu Quantum Technologies Inc.
//
// ap_axi_sdata.h - Vitis HLS's AXI-Stream word, as far as fpga-network-stack's C++ needs it
// outside __VITIS_HLS__ (it only includes the header there).
#pragma once

#include "ap_int.h"

template <int D, int U, int TI, int TD>
struct ap_axiu {
    ap_uint<D> data;
    ap_uint<(D + 7) / 8> keep;
    ap_uint<(D + 7) / 8> strb;
    ap_uint<1> last;
};
