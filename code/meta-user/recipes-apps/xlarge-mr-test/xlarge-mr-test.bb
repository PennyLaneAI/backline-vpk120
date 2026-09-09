DESCRIPTION = "Application to test huge memory registrations"
SECTION = "misc"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI ="\
    file://xlarge_mr_test.c \
"

DEPENDS += " rdma-core umm"
FILESEXTRAPATHS:prepend := "${THISDIR}/files/:" 
S = "${WORKDIR}"

TARGET_CC_ARCH += "${LDFLAGS}"

LIB_PATH = "${STAGING_DIR_TARGET}/usr/lib"
do_compile () {
    echo ${STAGING_DIR_TARGET} > ~/bblog
    ${CC} xlarge_mr_test.c -o xlarge_mr_test -lrdmacm -libverbs -lpthread -lumm
}

do_install () {
    export DIST_ROOT=${D}
    install -d ${D}${sbindir}
    install -m 0755 xlarge_mr_test ${D}${sbindir}/
}
