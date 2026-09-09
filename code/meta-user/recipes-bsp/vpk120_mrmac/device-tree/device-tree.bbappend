
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI:append = " file://system-user.dtsi \
		   file://versal-vpk120-revB.dtsi \
		   file://device_tree_ernic_tcl.patch"

do_configure:append() {
	echo "#include \"system-user.dtsi\"" >> "${DT_FILES_PATH}/system-top.dts"
        echo "#include \"versal-vpk120-revB.dtsi\"" >> "${DT_FILES_PATH}/system-top.dts"
}
