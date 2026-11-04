SUMMARY = "Xnvme HA Pktgen module example. It is for kernel v4.9 and above"

LICENSE = "GPLv2"
LIC_FILES_CHKSUM = "file://COPYING.GPL;md5=fcb02dc552a041dee27e4b85c7396067"

SRC_URI = "\
	file://COPYING.GPL \
	file://Makefile \
	file://axidma.c \
	file://axi_timer.c \
	file://main.c \
	file://mem.c \
	file://rnic.c \
	file://verbs.c \
	file://axidma.h \
	file://ib_verbs.h \
	file://rnic.h \
	file://xib_export.h \
	file://xib.h \
	file://xib_kmm_export.h \
	"
DEPENDS += "pl-allocator xib-kmm"
S = "${WORKDIR}"

inherit module
