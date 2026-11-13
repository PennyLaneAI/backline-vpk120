
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SYSTEM_USER_DTSI ?= "system-user.dtsi"
SRC_URI:append = " file://system-user.dtsi \
		   file://device_tree_ernic_tcl.patch"

do_configure:append() {
	sed -i '/memory@400000000 {/,/};/d' ${DT_FILES_PATH}/system-top.dts
	echo "#include \"system-user.dtsi\"" >> "${DT_FILES_PATH}/system-top.dts"
}
