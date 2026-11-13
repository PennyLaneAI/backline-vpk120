# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Xanadu Quantum Technologies Inc.
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# versal-vpk120-revB.dtsi (Xilinx, GPL-2.0) is not in this repo; build.sh copies it in from
# build/amd/sw.
SRC_URI:append = " file://system-user.dtsi \
                   file://versal-vpk120-revB.dtsi"

do_configure:append() {
	echo "#include \"system-user.dtsi\"" >> "${DT_FILES_PATH}/system-top.dts"
	echo "#include \"versal-vpk120-revB.dtsi\"" >> "${DT_FILES_PATH}/system-top.dts"
}
