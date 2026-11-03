#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Xanadu Quantum Technologies Inc.
#
# Build the PetaLinux 2024.2 image for backline-hsb.
#
#   source <petalinux>/settings.sh
#   sw/petalinux/build.sh <backline_hsb.xsa> <xilinx-vpk120-xsct-v2024.2-final.bsp> \
#                         <versal-vpk120-revB.dtsi> [out_dir]
#
# Creates out_dir/backline-hsb (default build/petalinux) from the VPK120 BSP, imports the
# XSA, adds sw/petalinux/meta-user (device tree, UIO kernel config, hwhs-tools, vpk-init),
# builds, packages BOOT.BIN and writes jtag-boot.tcl. The board dtsi is AMD's, which
# scripts/unpack-amd.sh puts in build/amd/sw.
set -euo pipefail

USAGE="usage: build.sh <xsa> <bsp> <board dtsi> [out_dir]"
XSA="$(cd "$(dirname "${1:?$USAGE}")" && pwd)/$(basename "$1")"
BSP="$(cd "$(dirname "${2:?$USAGE}")" && pwd)/$(basename "$2")"
DTSI="$(cd "$(dirname "${3:?$USAGE}")" && pwd)/$(basename "$3")"
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${4:-$REPO/build/petalinux}"
NAME=backline-hsb

command -v petalinux-create >/dev/null || { echo "ERROR: source the PetaLinux 2024.2 settings.sh first" >&2; exit 1; }
[ -f "$XSA" ] || { echo "ERROR: no XSA at $XSA" >&2; exit 1; }
[ -f "$BSP" ] || { echo "ERROR: no BSP at $BSP" >&2; exit 1; }
[ -f "$DTSI" ] || { echo "ERROR: no board dtsi at $DTSI (run make amd first)" >&2; exit 1; }

mkdir -p "$OUT"
cd "$OUT"
rm -rf "$NAME"
petalinux-create project -s "$BSP" -n "$NAME"
cd "$NAME"
petalinux-config --get-hw-description "$XSA" --silentconfig

META=project-spec/meta-user
SRC="$REPO/sw/petalinux/meta-user"

# Device tree: system-user.dtsi + the VPK120 rev B board dtsi
rm -rf "$META/recipes-bsp/device-tree"
cp -R "$SRC/recipes-bsp/device-tree" "$META/recipes-bsp/"
cp "$DTSI" "$META/recipes-bsp/device-tree/files/versal-vpk120-revB.dtsi"

# Kernel: generic-uio built in
mkdir -p "$META/recipes-kernel/linux/files"
cp "$SRC/recipes-kernel/linux/linux-xlnx_%.bbappend" "$META/recipes-kernel/linux/"
cp "$SRC/recipes-kernel/linux/files/uio.cfg" "$META/recipes-kernel/linux/files/"

# Apps: create (registers them in the rootfs config), then put our recipe and sources in
petalinux-create -t apps --template c -n hwhs-tools --enable --force
rm -rf "$META/recipes-apps/hwhs-tools"
cp -R "$SRC/recipes-apps/hwhs-tools" "$META/recipes-apps/"
cp "$REPO/sw/lib/hwhs/hwhs.c" "$REPO/sw/lib/hwhs/hwhs.h" \
   "$REPO/common/include/hwhs_regs.h" "$REPO/common/include/hsb_regs.h" "$REPO/common/include/hsb_handshake.h" \
   "$REPO/sw/apps/hsbctl/hsbctl.c" "$REPO/sw/apps/mrmac-up/mrmac-up.c" \
   "$META/recipes-apps/hwhs-tools/files/"

petalinux-create -t apps --template install -n vpk-init --enable --force
rm -rf "$META/recipes-apps/vpk-init"
cp -R "$SRC/recipes-apps/vpk-init" "$META/recipes-apps/"

# Logins as on the ERNIC build (../ernic_hwhs)
ROOTFS_CONFIG="project-spec/configs/rootfs_config"
USERS='CONFIG_ADD_EXTRA_USERS="root:root;petalinux:2024;"'
if grep -q "CONFIG_ADD_EXTRA_USERS" "$ROOTFS_CONFIG"; then
    sed -i "s|^.*CONFIG_ADD_EXTRA_USERS.*|$USERS|" "$ROOTFS_CONFIG"
else
    echo "$USERS" >> "$ROOTFS_CONFIG"
fi

sed -i 's/^# CONFIG_libstdcPLUSPLUS is not set/CONFIG_libstdcPLUSPLUS=y/' "$ROOTFS_CONFIG"
printf 'DISTRO_FEATURES:remove = " cairo"\nDISTRO_FEATURES:remove = " x11"\n' >> "$META/conf/petalinuxbsp.conf"
petalinux-config -c rootfs --silentconfig

petalinux-build
petalinux-package --boot --format BIN --plm --psmfw --u-boot --dtb --force
petalinux-boot jtag --kernel images/linux/Image --rootfs images/linux/rootfs.cpio.gz.u-boot --tcl jtag-boot.tcl

sed -i -e 's/\*PMC\*/PMC/g' \
    -e '1a after 2000\ntargets -set -nocase -filter {name =~ "*Versal*"}\nmwr 0xf1260200 0x0100\nrst -system\nafter 3000' \
    jtag-boot.tcl

echo "BUILD SUCCESS: $OUT/$NAME/images/linux, boot with: (cd $OUT/$NAME && xsdb jtag-boot.tcl)"
