DESCRIPTION = "rping test application"
SECTION = "misc"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI ="\
    file://Makefile \
    file://inv_imm_test_in.c \
    file://inv_imm_test_out.c \
    file://inv_imm.h \
"

DEPENDS += " rdma-core umm"
FILESEXTRAPATHS:prepend := "${THISDIR}/files/:" 
S = "${WORKDIR}"

TARGET_CC_ARCH += "${LDFLAGS}"

LIB_PATH = "${STAGING_DIR_TARGET}/usr/lib"
do_compile () {
	echo ${STAGING_DIR_TARGET} > ~/bblog
    ${CC} inv_imm_test_out.c -o inv_imm_test_out -lrdmacm -libverbs -lpthread -lumm
    ${CC} inv_imm_test_in.c -o inv_imm_test_in -lrdmacm -libverbs -lpthread -lumm
}

do_install () {
    export DIST_ROOT=${D}
    install -d ${D}${sbindir}
    install -m 0755 inv_imm_test_in ${D}${sbindir}/
    install -m 0755 inv_imm_test_out ${D}${sbindir}/
}
