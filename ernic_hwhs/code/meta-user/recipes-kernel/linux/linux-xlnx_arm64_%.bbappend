inherit kernel

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
RM_WORK_EXCLUDE += "linux-xlnx"

SRC_URI:append = " file://0001-MRMAC-100G-support.patch \
	     file://002-Adding-xib-abi-header.patch \
	     file://0001-xib-nvmf-addons.patch \
	     file://0001-removed-warn_on-on-disconnect.patch \
	     file://0001-Support-to-enable-hw-accl.patch \
	     file://0001-Add-RDMA-driver-ID-for-ERNIC.patch \
	     file://0001-arm64-zone-dev-support.patch \
	     file://0001-Add-new-rdma-core-verb-ibv_reg_mr_ex.patch \
	     file://0001-support-for-hw-hs-qp-attr.patch \
         file://0001-update-to-linux-6-6.patch \
         file://fix-mrmac-issues.patch \
         file://fill_subsection_map.patch \
		 file://update_to_linux_6_6_40_239b09.patch \
                 file://standard_bits.patch \
         "

SRC_URI:append = " file://arm64_ernic.cfg"
