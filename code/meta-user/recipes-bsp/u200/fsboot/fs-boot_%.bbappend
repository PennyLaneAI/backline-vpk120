FILESEXTRAPATHS:prepend := "${THISDIR}:"
SRC_URI:append = " \
                   file://fs-boot_flash_boot.patch"

do_configure:append () {
	hsmoutf="${WORKDIR}/offsets"
	touch ${hsmoutf}
	ipinfo="${TOPDIR}/../components/yocto/layers/meta-xilinx/meta-xilinx-core/gen-machine-conf/gen-machine-scripts/data/ipinfo.yaml"
	petalinux_hsm="${TOPDIR}/../components/yocto/layers/meta-xilinx/meta-xilinx-core/gen-machine-conf/gen-machine-scripts/petalinux_hsm.tcl"
	sysconfig_path="${TOPDIR}/../project-spec/configs"
	
	xsct -sdx -nodisp "${petalinux_hsm}"\
		"get_flash_width_parts" "${sysconfig_path}/config" "${ipinfo}" \
		"${XSCTH_HDF}" "${hsmoutf}"
}

do_compile:prepend () {
	boot_offset=$(egrep -e "^boot=" "${hsmoutf}" | cut -d "=" -f 2 | cut -d " " -f 1)
}
