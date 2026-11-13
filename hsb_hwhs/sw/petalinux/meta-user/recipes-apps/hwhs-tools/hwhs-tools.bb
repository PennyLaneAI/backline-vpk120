# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Xanadu Quantum Technologies Inc.
SUMMARY = "hwhs-hsb tools for the VPK120: hsbctl, mrmac-up"
SECTION = "PETALINUX/apps"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

# sw/petalinux/build.sh copies these in from sw/lib, sw/apps and common/include
SRC_URI = " \
    file://Makefile \
    file://hwhs.c \
    file://hwhs.h \
    file://hwhs_regs.h \
    file://hsb_regs.h \
    file://hsb_handshake.h \
    file://hsbctl.c \
    file://mrmac-up.c \
"

S = "${WORKDIR}"

do_compile() {
    oe_runmake
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${S}/hsbctl ${D}${bindir}/hsbctl
    install -m 0755 ${S}/mrmac-up ${D}${bindir}/mrmac-up
}
