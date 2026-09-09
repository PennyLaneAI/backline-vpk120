inherit kernel

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
RM_WORK_EXCLUDE += "linux-xlnx"

SRC_URI:append = " file://0001-cmac-100G.patch \
	     file://002-Adding-xib-abi-header.patch \
	     file://0001-xib-nvmf-addons.patch \
	     file://0001-removed-warn_on-on-disconnect.patch \
	     file://0001-Support-to-enable-hw-accl.patch \
	     file://0001-Add-RDMA-driver-ID-for-ERNIC.patch \
	     file://0001-Add-new-rdma-core-verb-ibv_reg_mr_ex.patch \
	     file://0001-support-for-hw-hs-qp-attr.patch \
		 file://0001-ublaze-update-to-linux-6-6.patch \
		 file://revert_xilinx_axienet_to_2024_1.patch \
                 file://2024_2_fixes.patch \
                 file://fix_uverbs_bug.patch \
                 file://standard_bits.patch \
		"

SRC_URI:append = "file://microblaze_ernic.cfg"