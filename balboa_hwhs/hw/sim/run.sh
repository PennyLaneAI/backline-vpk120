#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Xanadu Quantum Technologies Inc.
#
# hw/sim/run.sh [lint|sim|all]   (default: all)
#
#   lint  Verilator lint of backline_roce_top with BALBOA's vendored RTL, hwhs, and stand-ins
#         for what only Vivado and Vitis HLS produce (lint_stubs.sv: platform_wrapper, AMD's
#         mrmac_0_exdes, BALBOA's HLS cores, two Vivado IPs; xip_models.sv: FIFOs and slices)
#   sim   tb_hwhs_engine: the engine alone, every corner its FSM has
#         tb_hwhs_balboa: hwhs and balboa_net against BALBOA's roce_stack (vendored), with a
#                         model of the rocev2 HLS core's user side
#         tb_net:         eth_pad, icmp_server_ip and the TX stream merges
#         All run with --timing and --assert; hwhs_engine_sva.sv is bound into the engine.
#
# Needs Verilator >= 5.0 (brew install verilator / apt install verilator).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
OUT="${OUT:-$REPO/build/sim}"
mkdir -p "$OUT"

C="$REPO/third_party/balboa/coyote/hw/hdl"
# Coyote's RTL BALBOA needs, in compile order (lynx_pkg first: it carries the defines)
COYOTE=(
    "$REPO/hw/rtl/balboa_cfg/lynx_pkg.sv"
    "$C/pkg/axi_intf.sv" "$C/pkg/lynx_intf.sv"
    "$C/common/regs/axis_reg.sv" "$C/common/regs/axis_reg_array.sv"
    "$C/common/regs/meta_reg.sv" "$C/common/regs/meta_reg_array.sv"
    "$C/common/queues/fifo.sv" "$C/common/queues/meta_queue.sv"
    "$C/common/queues/queue_meta.sv" "$C/common/queues/queue_stream.sv"
    "$C/common/ram/sp_ram_nc.sv"
    "$C"/network/rdma/*.sv
)
# SYNTHESIS: the interfaces' simulation-only clocking blocks need Coyote's testbench timing
COYOTE_OPTS=(+define+SYNTHESIS +incdir+"$C/pkg")
HWHS=("$REPO"/hw/rtl/hwhs/*.v)
WARN=(-Wno-fatal -Wno-lint -Wno-style -Wno-WIDTH -Wno-UNOPTFLAT -Wno-TIMESCALEMOD)
# .v files are Verilog-2005, as Vivado reads them
V2005=(+1364-2005ext+v)

what="${1:-all}"

if [ "$what" = lint ] || [ "$what" = all ]; then
    echo "== lint backline_roce_top"
    verilator --lint-only "${V2005[@]}" -Wall -Wno-fatal -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL -Wno-UNUSEDPARAM \
        -Wno-UNDRIVEN -Wno-PINCONNECTEMPTY -Wno-TIMESCALEMOD -Wno-PROCASSINIT "${COYOTE_OPTS[@]}" \
        --top-module backline_roce_top "${COYOTE[@]}" "$C/network/stack/network_stack.sv" \
        "$HERE/xpm_models.sv" "$HERE/xip_models.sv" "$HERE/lint_stubs.sv" \
        "$REPO"/hw/rtl/balboa_cfg/icmp_server_ip.sv "$REPO"/hw/rtl/balboa_cfg/vio_ip.sv \
        "$REPO"/hw/rtl/balboa_cfg/axis_interconnect_512.sv "${HWHS[@]}" \
        "$REPO"/hw/rtl/top/eth_pad.v "$REPO"/hw/rtl/top/balboa_net.sv "$REPO"/hw/rtl/top/backline_roce_top.sv \
        2>&1 | grep -E '^%(Error|Warning)' | grep -v 'third_party/' | grep -v 'balboa_cfg/lynx_pkg.sv' || true
    echo "== lint transport_demo"
    verilator --lint-only "${V2005[@]}" -Wall -Wno-fatal -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL "$REPO/hw/rtl/demo/transport_demo.v" \
        2>&1 | grep -E '^%Error' || true
fi

if [ "$what" = sim ] || [ "$what" = all ]; then
    echo "== build tb_hwhs_engine"
    verilator --binary --timing --assert -j 0 "${V2005[@]}" "${WARN[@]}" --top-module tb_hwhs_engine -Mdir "$OUT/obj_engine" \
        "$REPO/hw/rtl/hwhs/hwhs_engine.v" "$HERE/hwhs_engine_sva.sv" "$HERE/tb_hwhs_engine.sv" \
        -o tb_hwhs_engine > "$OUT/build_engine.log" 2>&1 || { tail -40 "$OUT/build_engine.log"; exit 1; }
    echo "== run tb_hwhs_engine"
    "$OUT/obj_engine/tb_hwhs_engine"

    echo "== build tb_hwhs_balboa"
    verilator --binary --timing --assert -j 0 "${V2005[@]}" "${WARN[@]}" "${COYOTE_OPTS[@]}" --top-module tb_hwhs_balboa \
        -Mdir "$OUT/obj_balboa" "${COYOTE[@]}" "$HERE/network_stack_sim.sv" "$HERE/rocev2_model.sv" \
        "$HERE/xpm_models.sv" "$HERE/xip_models.sv" "${HWHS[@]}" "$REPO/hw/rtl/top/balboa_net.sv" \
        "$HERE/hwhs_engine_sva.sv" "$HERE/tb_hwhs_balboa.sv" -o tb_hwhs_balboa > "$OUT/build_balboa.log" 2>&1 \
        || { tail -40 "$OUT/build_balboa.log"; exit 1; }
    echo "== run tb_hwhs_balboa"
    "$OUT/obj_balboa/tb_hwhs_balboa"

    echo "== build tb_net"
    verilator --binary --timing --assert -j 0 "${V2005[@]}" "${WARN[@]}" --top-module tb_net -Mdir "$OUT/obj_net" \
        "$REPO/hw/rtl/top/eth_pad.v" "$REPO/hw/rtl/balboa_cfg/icmp_server_ip.sv" \
        "$REPO/hw/rtl/balboa_cfg/axis_interconnect_512.sv" "$HERE/tb_net.sv" \
        -o tb_net > "$OUT/build_net.log" 2>&1 || { tail -40 "$OUT/build_net.log"; exit 1; }
    echo "== run tb_net"
    "$OUT/obj_net/tb_net"
fi
