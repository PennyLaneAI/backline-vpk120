SUMMARY = "Infiniband Verbs Performance tests"
DESCRIPTION = "This is the userspace application for performace tests with OFED"

DEPENDS = "rdma-core umm"

#SRC_URI = "git://github.com/linux-rdma/perftest.git; \
#	   file://0001_get_clock_microblaze.patch;  "
#SRC_URI[md5sum] = "410ed961fb5cc871bf6171100ac7d77897e4f21d"
#SRCREV = "410ed961fb5cc871bf6171100ac7d77897e4f21d"
EXTRA_OECONF = "--host microblazeel-xilinx-linux-gnu "

SRC_URI = "git://github.com/lsgunth/perftest.git; \
	   file://0001_get_clock_microblaze.patch; \
	   file://0001-xib-perftest-support.patch; \
	   file://0002-ipv6-support.patch; \
	   file://0001-umm-perftest-patch.patch; \
	   file://0001-microblaze-get-cycles-BW.patch; \
	   file://0001-Fixed-compilation-error.patch; \
	   file://0001-enable_runall_option_ERNIC.patch; \
           file://write_bw_dd.patch; \
           file://send_bw_delay.patch; \
           file://send_rq_depth.patch; \
           file://adjust_rx_depth.patch \
           "

#file://0002-max-payload-support-fix.patch;
SRC_URI[md5sum] = "b2f2e82e3e507769896aee382e112e3515c913db"
SRCREV = "b2f2e82e3e507769896aee382e112e3515c913db"
S = "${WORKDIR}/git"

inherit autotools

RM_WORK_EXCLUDE += "perftest"

EXTRA_OECONF = "--host microblazeel-xilinx-linux-gnu --prefix=/usr"

CFLAGS:append := " -D_GNU_SOURCE "

do_configure () {
        #${S}/autogen.sh
        cd ${S}
	autoreconf -fi
	./configure ${EXTRA_OECONF}
}

do_compile () {
        cd ${S}
        oe_runmake
}

do_install () {
        cd ${S}
	oe_runmake install DESTDIR=${D}
}
#do_install () {
#	install -D ${S}/ib_read_bw  ${D}/${bindir}
#	install -D ${S}/ib_write_bw ${D}/${bindir}
#	install -D ${S}/ib_send_bw  ${D}/${bindir}
#}
#Default Dual License https://github.com/linux-rdma/rdma-core/blob/master/COPYING.md
LICENSE = "GPLv2"
LIC_FILES_CHKSUM = "file://COPYING;md5=9310aaac5cbd7408d794745420b94291"
