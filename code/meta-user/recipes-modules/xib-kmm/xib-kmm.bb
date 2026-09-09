SUMMARY = "Xnvme HA Pktgen module example. It is for kernel v4.9 and above"

LICENSE = "GPLv2"
LIC_FILES_CHKSUM = "file://COPYING.GPL;md5=fcb02dc552a041dee27e4b85c7396067"

SRC_URI = "\
	file://COPYING.GPL \
	file://Makefile \
	file://xib_kmm.c \
	file://xib_kmm_ioctl.h \
	file://xib_kmm.h \
	file://xib_kmm_export.h \
	"

S = "${WORKDIR}"
DEPENDS += "pl-allocator"
inherit module
