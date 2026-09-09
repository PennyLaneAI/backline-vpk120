# VPK120 Board Setup and Configuration Guide

## Overview
This README provides instructions and scripts for programming the AMD Versal™ Premium Evaluation Kit (VPK120) and configuring its PetaLinux network interfaces for use with backline.

Choose one of the following paths:

- **Use the release images (recommended):** Download the prebuilt package and program the
  board directly. You do not need to rebuild Vivado or PetaLinux images.
- **Rebuild the images (optional):** Start from the AMD ERNIC reference design, apply the
  Backline patch, and rebuild the hardware and PetaLinux images.

After completing either path, continue with [Serial Console & Login](#serial-console--login)
and [Network Setup](#network-setup).

### Licensing and Compliance

Individual subsystems within this repository are licensed under their respective directories.

- **AMD Vivado License:** Must include support for Versal Premium devices (VPK120).
- **Reference Design Sources:** The AMD ERNIC MRMAC reference design remains the intellectual property of Advanced Micro Devices, Inc. Users must source the package directly from AMD. This repository provides modifications as a patch file (`backline.patch`).
- **AMD ERNIC IP License:** Rebuilding the design requires an active [AMD ERNIC IP core license](https://www.amd.com/en/products/adaptive-socs-and-fpgas/intellectual-property/ef-di-ernic.html).
- **AMD Versal 100G Multirate Ethernet MAC IP License:** Rebuilding the design requires an active AMD [100G Multirate Ethernet MAC IP license](https://www.amd.com/en/products/adaptive-socs-and-fpgas/intellectual-property/mrmac.html).
- **HDL patch:** All HDL patches and scripts under `/hdl` are licensed under the Apache License, Version 2.0.
- **Software Licenses:** All software under `/code`, including but not limited to Linux drivers and PetaLinux components are subject to AMD PetaLinux EULA and relevant scoped open-source licenses (GPLv2, MIT, Apache 2.0).

### ERNIC SW Code

The provided SW code is a modified fork of the ERNIC SW repo for CSE/PPSG.

It is based upon the ERNIC code that was originally developed by DCG Storage team and was released on the ERNIC lounge area in 2021.


### Folder Layout
```shell
├── hdl
└── code
   └── meta-user
       ├── conf
       ├── recipes-apps
       │   ├── gpio-demo
       │   ├── inv-imm-test-app
       │   ├── misc
       │   ├── peekpoke
       │   ├── perftest
       │   ├── umm
       │   ├── xhw-hs-client
       │   ├── xhw-hs-server
       │   ├── xlarge-mr-test
       │   └── xrping
       ├── recipes-bsp
       │   ├── u200
       │   │   ├── device-tree
       │   │   ├── fsboot
       │   │   ├── u-boot
       │   │   └── uboot-device-tree
       │   └── vck5000
       │       ├── device-tree
       │       ├── u-boot
       │       └── uboot-device-tree
       ├── recipes-kernel
       │   └── linux
       ├── recipes-modules
       │   ├── hw-hs
       │   ├── pl-allocator
       │   ├── xib-kmm
       │   ├── xib-module
       │   └── xkperftest-server
       └── recipes-support
           └── rdma-core

```

| Directory     | Description |
| ---      | ---       |
| /hdl | Backline RTL patches and unpacking scripts |
| /code   | Application source code. Kernel, Module and BSP patches 
| /code/recipes-apps   | Bitbake and source code for user level applications 
| /code/recipes-bsp   | Board Support Package for U200 and VCK5000. Device Tree and U-Boot |
| /code/recipes-kernel | Applications | Patches and Bitbake files for both ARM and AMD64 processors. Specific files are selected  build_peta_proj 
| /code/recipes-modules | Kernel Module Bitbake and source files
| /code/recipes-support | Patches to https://github.com/linux-rdma/rdma-core 

## Prerequisites

### Common

- **Hardware:** AMD Versal VPK120 board, Micro-USB or USB-C cable for JTAG/UART, and
  Ethernet cable(s).
- **Software:** AMD Vivado™ Design Suite 2024.2 installed on the host PC. The `xsdb` executable
  must be available after sourcing the Vivado environment.

### Rebuild only

- AMD PetaLinux 2024.2.
- The VPK120 XSCT 2024.2 BSP.
- AMD **ERNIC VPK120 MRMAC reference design**, v1.3
  (release `ERNIC-2024_2_VPK120_RC6`), downloaded as
  `reference design VPK120_mrmac_rev_1.3.zip`.

## Use the prebuilt release images

Download and extract `hw_ready_for_download_v0.1.0.zip`. The package contains:

```text
hw_ready_for_download_v0.1.0/
├── README.md
├── jtag-boot.tcl
└── Images/
    └── Linux/
        └── (PetaLinux boot images)
```

Keep `jtag-boot.tcl` beside the `Images/` directory and run it from the extracted package root:

```bash
cd hw_ready_for_download_v0.1.0
source /path/to/Vivado/2024.2/settings64.sh
xsdb jtag-boot.tcl
```

Before running the command, set the board boot-mode switches to **JTAG mode** and power-cycle the
board.

If the JTAG cable permissions are not configured in the host's udev rules, `xsdb` may need to be
run with `sudo`.

After the board boots, continue with [Serial Console & Login](#serial-console--login).

## Rebuild the images (optional)

Ensure these three files are placed in the same directory (default `/hdl`):

- `reference design VPK120_mrmac_rev_1.3.zip`
- `backline.patch`
- `unpack-ernic-vpk120.sh`

Within that same directory, unpack the AMD reference design and apply the Backline patch:

```bash
bash unpack-ernic-vpk120.sh -p
```

This creates the patched source tree in `ernic-vpk120/`. Configure the build environment and build
the images from that directory:

```bash
cd ernic-vpk120
source /path/to/Vivado/2024.2/settings64.sh
source /path/to/petalinux/settings.sh

export BSP_PATH=/path/to/xilinx-vpk120-xsct-v2024.2-final.bsp
export VPK_INIT_CONFIG_DIR="$PWD/sw/vpk_init"

make vivado_build JOBS=<N> CLEAN=1
make peta_build
make jtag-boot
```

`make jtag-boot` generates the boot script in the PetaLinux project. To program the board, set its
boot-mode switches to **JTAG mode**, power-cycle it, and run:

```bash
make boot-fpga
```

The combined `make full JOBS=<N>` target builds both images, generates `jtag-boot.tcl`, and programs
the board. It should only be used when the board is connected and ready to boot.

## Serial Console & Login

1. Connect to the board's serial port using a terminal emulator such as Minicom, Tera Term, or
   Picocom.
2. Log in to PetaLinux:
   * **Default Username:** `petalinux`
   * **Default Password:** `2024`

Change the default password when the board is placed on a shared or routed network.

## Network Setup

The following addresses match the Backline reference setup. Replace them when using a different
network. Run these commands in the board's PetaLinux terminal:

```bash
sudo ifconfig eth0 192.168.1.1 mtu 4200 up
sudo ip addr add 192.168.3.15/24 dev end0
sudo ip link set end0 up
```

Replace `XX:XX:XX:XX:XX:XX` with your target host's physical MAC address:
```bash
sudo ip neigh replace 192.168.1.2 lladdr XX:XX:XX:XX:XX:XX dev eth0 nud permanent
```

Versal, and Vivado, are trademarks of Advanced Micro Devices, Inc.
