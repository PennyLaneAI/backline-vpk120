SUMMARY = "Kernel space application for performance tests"

LICENSE = "GPLv2"
LIC_FILES_CHKSUM = "file://COPYING.GPL;md5=fcb02dc552a041dee27e4b85c7396067"

SRC_URI = "\
	file://COPYING.GPL \
	file://Makefile \
	file://xkperftest_server.c \
	file://xib_kmm_export.h \
	"

S = "${WORKDIR}"

DEPENDS += " xib-kmm"
RPROVIDES:${PN} += "xperftest-server"
inherit module
