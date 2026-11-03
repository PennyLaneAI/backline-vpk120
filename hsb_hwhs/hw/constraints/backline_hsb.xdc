# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Xanadu Quantum Technologies Inc.
#
# backline_hsb.xdc - constraints
#
# HSB's data (hif), control (apb) and timer (ptp) clocks all come from clk_wizard_hsb, one
# MMCM, so Vivado would time the paths between them as synchronous. HSB crosses between them
# only through its own synchronizers (data_sync, reg_cdc, dc_fifo_generic). This turns off timing
# between the three.

set hif_clk [get_clocks -of_objects [get_nets hif_clk]]
set apb_clk [get_clocks -of_objects [get_nets apb_clk]]
set ptp_clk [get_clocks -of_objects [get_nets ptp_clk]]

set_clock_groups -asynchronous -group $hif_clk -group $apb_clk -group $ptp_clk
