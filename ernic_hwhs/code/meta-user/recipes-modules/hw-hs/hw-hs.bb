SUMMARY = "ERNIC HW HS driver recipe"

LICENSE = "GPLv2"
LIC_FILES_CHKSUM = "file://COPYING.GPL;md5=fcb02dc552a041dee27e4b85c7396067"

SRC_URI = "\
	file://COPYING.GPL \
	file://Makefile \
	file://hw_hs.c \
	file://hw_hs_ioctl.h \
	file://hw_hs.h \
	file://rnic.h \
	file://xib_export.h \
	"

S = "${WORKDIR}"

DEPENDS += "xib-module"
inherit module
