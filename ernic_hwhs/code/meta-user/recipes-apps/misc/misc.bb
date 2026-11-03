DESCRIPTION = "System/board based miscellaneous configuration"
SECTION = "misc"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI ="\
     file://blacklist.conf \
"

FILESEXTRAPATHS:prepend := "${THISDIR}/files/:"
S = "${WORKDIR}"

do_install () {
    export DIST_ROOT=${D}
    install -d ${D}/etc/modprobe.d
    install -m 0755 blacklist.conf ${D}/etc/modprobe.d
}
