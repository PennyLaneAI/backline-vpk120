SUMMARY = "Infiniband Verbs Performance tests"
DESCRIPTION = "This is the userspace application for performace tests with OFED"
LICENSE = "GPL|BSD"

SRC_URI = "git://github.com/linux-rdma/perftest.git;branch=master \
           "

S = "${WORKDIR}/perftest"
SRCREV = "${AUTOREV}"

RM_WORK_EXCLUDE += "perftest"

#Default Dual License https://github.com/linux-rdma/rdma-core/blob/master/COPYING.md
#LICENSE = "GPLv2"
#LIC_FILES_CHKSUM = "file://COPYING;md5=b234ee4d69f5fce4486a80fdaf4a4263"

do_compile () {
	oe_runmake
}
