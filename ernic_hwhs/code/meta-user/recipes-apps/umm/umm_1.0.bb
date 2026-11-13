SUMMARY = "user memory manager"
DESCRIPTION = "This is the memory manager in the user space"
SECTION = "libs"

LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"
FILESEXTRAPATHS:prepend := "${THISDIR}/files/:"

DEPENDS = "libnl"
RDEPENDS_${PN} = "bash perl"

SRC_URI = "\
	   file://umm.h \
	   file://umm_export.h \
           file://umm.c \
           file://bit_ops.c \
           file://bit_ops.h \
           file://Makefile \
           "
SRCREV = "7844b3fbe5120623d63b29ecb43eb83a61129658"
S = "${WORKDIR}"

TARGET_CC_ARCH += "${LDFLAGS}"
LIB_PATH = "${STAGING_DIR_TARGET}/usr/lib"

FILES_SOLIBSDEV = ""
FILES:${PN} += "${libdir}/libumm.so"
INSANE_SKIP_${PN} += "dev-so"

do_compile () {
	oe_runmake
}

do_install() {
	install -d ${D}${libdir}
	install -d ${D}${includedir}
	oe_soinstall ${S}/libumm.so.${PV} ${D}${libdir}
	install -m 0755 ${S}/umm_export.h ${D}${includedir}/
	install -m 0755 ${S}/libumm.so ${D}${libdir}/
}

do_stage () {
    install -m 0644 ${WORKDIR}/umm_export.h ${D}${includedir}
}
