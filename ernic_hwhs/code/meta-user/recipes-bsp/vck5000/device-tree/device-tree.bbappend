
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI:append = " file://system-user.dtsi \
		   file://versal-vck5000-reva.dtsi \
		   file://device_tree_ernic_tcl.patch"

do_configure:append() {
	echo "#include \"system-user.dtsi\"" >> "${DT_FILES_PATH}/system-top.dts"
        echo "#include \"versal-vck5000-reva.dtsi\"" >> "${DT_FILES_PATH}/system-top.dts"
}
