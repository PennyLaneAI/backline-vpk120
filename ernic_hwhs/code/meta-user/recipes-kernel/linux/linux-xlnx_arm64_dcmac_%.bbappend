inherit kernel

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
RM_WORK_EXCLUDE += "linux-xlnx"

SRC_URI:append = " file://arm64_ernic.cfg "
SRC_URI:append = " file://002-Adding-xib-abi-header.patch "
SRC_URI:append = " file://0001-xib-nvmf-addons.patch "
SRC_URI:append = " file://0001-removed-warn_on-on-disconnect.patch "
SRC_URI:append = " file://0001-Support-to-enable-hw-accl.patch "
SRC_URI:append = " file://0001-Add-RDMA-driver-ID-for-ERNIC.patch "
SRC_URI:append = " file://0001-arm64-zone-dev-support.patch "
SRC_URI:append = " file://0001-Add-new-rdma-core-verb-ibv_reg_mr_ex.patch "
SRC_URI:append = " file://0001-support-for-hw-hs-qp-attr.patch "
SRC_URI:append = " file://vpk120-update-to-linux-6-6.patch "
SRC_URI:append = " file://standard_bits.patch "
SRC_URI:append = " file://cx5_fix.patch "
SRC_URI:append = " file://dcmac_align_delay.patch "