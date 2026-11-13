# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Xanadu Quantum Technologies Inc.
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# versal-vpk120-revB.dtsi is Xilinx's GPL-2.0 board dtsi, verbatim. It is not in this repo:
# sw/petalinux/build.sh copies it from build/amd/sw (make amd). The include order is the one the
# ERNIC build booted with.
SRC_URI:append = " file://system-user.dtsi \
                   file://versal-vpk120-revB.dtsi"

do_configure:append() {
	echo "#include \"system-user.dtsi\"" >> "${DT_FILES_PATH}/system-top.dts"
	echo "#include \"versal-vpk120-revB.dtsi\"" >> "${DT_FILES_PATH}/system-top.dts"
}
