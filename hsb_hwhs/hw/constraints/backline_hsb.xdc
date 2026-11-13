# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Xanadu Quantum Technologies Inc.
#
# backline_hsb.xdc - clock groups.
#
# hif, apb and ptp come from one MMCM, but HSB crosses between them only through its own
# synchronizers, so they are timed as asynchronous.

set hif_clk [get_clocks -of_objects [get_nets hif_clk]]
set apb_clk [get_clocks -of_objects [get_nets apb_clk]]
set ptp_clk [get_clocks -of_objects [get_nets ptp_clk]]

set_clock_groups -asynchronous -group $hif_clk -group $apb_clk -group $ptp_clk
