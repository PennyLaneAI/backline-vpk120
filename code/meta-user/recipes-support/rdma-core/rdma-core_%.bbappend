DEPENDS += " umm"
FILESEXTRAPATHS:prepend := "${THISDIR}/:"
SRC_URI += " file://0001-xilinx-RNIC-providers-code.patch \
	     file://0000-rping-modifications.patch \
	     file://0001-new-verb-reg-mr-ex.patch \
	     file://0001-ERNIC-v3.0-patch.patch \
	     file://0001-ERNIC-v3.1-patch.patch \
	     file://0001-Changes-for-versal.patch \
	     file://0002-ERNIC-SQD-support.patch \
	     file://0001-Corrected-xib_u_reg_mr-function-prototype.patch \
             file://0002-rdma-core-42.0-r0.patch \
             file://ERNIC_V4.0.patch \
             file://ERNIC_V4.0.1.patch \
             file://add_query_device_ex.patch \
             file://ERNIC_V4.2.patch \
             file://driver_id_change.patch \
             file://0001-rc_pingpong_modified_for_ERNIC.patch \
             file://0001-Fix_for_error_completions_poll_cq.patch \
             file://ERNIC_V4.2.1.patch \
             file://Fix_reg_mr_bug.patch \
           "
