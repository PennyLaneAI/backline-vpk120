FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

BOOTMODE = "jtag"
BOOTFILE_EXT = ".versal"
SRC_URI += " \
            file://boot.cmd.jtag.versal \
            "
