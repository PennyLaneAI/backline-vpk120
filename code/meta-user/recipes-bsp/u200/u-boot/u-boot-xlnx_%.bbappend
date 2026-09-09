FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI += "file://platform-top.h \
	        file://add_do_irqinfo.patch \
			"

do_configure:append () {
	if [ "${U_BOOT_AUTO_CONFIG}" = "1" ]; then
		sysconfig_path="${TOPDIR}/../project-spec/configs"
		install ${sysconfig_path}/u-boot-xlnx/platform-auto.h ${S}/include/configs/
		install ${WORKDIR}/platform-top.h ${S}/include/configs/
	fi
}

do_configure:append_microblaze () {
	if [ "${U_BOOT_AUTO_CONFIG}" = "1" ]; then
		install -d ${B}/source/board/xilinx/microblaze-generic/
		install ${WORKDIR}/config.mk ${B}/source/board/xilinx/microblaze-generic/
	fi
}
