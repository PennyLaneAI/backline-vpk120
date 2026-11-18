# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Xanadu Quantum Technologies Inc.
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# The board fragment (versal-vpk120-revb.dtsi) comes from the stock DTG flow: the VPK120 BSP sets
# CONFIG_SUBSYSTEM_MACHINE_NAME="versal-vpk120-revb", so the generated system-top.dts includes it
# from device-tree-xlnx, followed by pcw.dtsi and our system-user.dtsi.
SRC_URI:append = " file://system-user.dtsi"
