# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Xanadu Quantum Technologies Inc.
SUMMARY = "VPK120 boot setup for backline-hsb: management network, UIO binding, 100G link"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

SRC_URI = " \
    file://vpk-init.sh \
    file://vpk-init.service \
"

S = "${WORKDIR}"

inherit systemd

RDEPENDS:${PN} += "hwhs-tools"
SYSTEMD_PACKAGES = "${PN}"
SYSTEMD_SERVICE:${PN} = "vpk-init.service"
SYSTEMD_AUTO_ENABLE = "enable"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${S}/vpk-init.sh ${D}${bindir}/vpk-init.sh

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${S}/vpk-init.service ${D}${systemd_system_unitdir}/vpk-init.service
}
