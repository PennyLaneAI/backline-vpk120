# VPK120 Board Setup and Configuration Guide (HSB)

## Overview
This README provides instructions and scripts for building the NVIDIA Holoscan Sensor Bridge (HSB)
design for the AMD Versal™ Premium Evaluation Kit (VPK120), programming the board, and bringing up
its link to the coprocessor for use with backline.

The design places Backline's hardware-handshake controller (hwhs-hsb) in front of NVIDIA's HSB FPGA
IP. The images are built from source:

1. [Build the images](#build-the-images) from AMD's reference design and this folder.
2. [Program the board](#program-the-board) over JTAG.
3. Continue with [Serial Console & Login](#serial-console--login),
   [Network Setup](#network-setup), [Bring-up Checks](#bring-up-checks) and
   [Run a Session](#run-a-session).

### How a Round Works

One round: the A72 (or `transport_demo`, in hw-loop mode) puts a 64 B request in the slot RAM and
starts the engine. hwhs stamps a sequence number into the request and hands it to HSB sensor-RX 0.
HSB sends it to the coprocessor as a UC RDMA WRITE into its ring, which needs the bitstream built
with [`patches/0001`](patches/README.md); stock HSB sends a WRITE_WITH_IMM, which `hsb-echo -I` and
`hsb-gpu-echo -I` ask for instead. The coprocessor answers with a UC SEND to QP 2. HSB delivers that payload on sensor-TX 0, and hwhs matches the sequence
number and records the reply and the RTT.

The A72 configures HSB with ECB packets (UDP 8192). Our mailbox
injects them into HSB's second host port.

### Licensing and Compliance

Individual subsystems within this folder are licensed under their respective directories.

- **AMD Vivado License:** Must include support for Versal Premium devices (VPK120).
- **Reference Design Sources:** The AMD ERNIC MRMAC reference design remains the
  intellectual property of Advanced Micro Devices, Inc. Users must download it directly from AMD. `make amd` unpacks the parts the build reads into `build/amd`:
  - for Vivado: the CIPS, NoC and MRMAC Tcl, the MRMAC example design RTL, the clock wizard and the XDCs;
  - for PetaLinux: the BSP and the VPK120 rev B board device tree (`versal-vpk120-revB.dtsi`,
    GPL-2.0, © Xilinx). `sw/petalinux/build.sh` copies the dtsi into the image's device-tree
    recipe.
- **AMD Versal 100G Multirate Ethernet MAC IP License:** Rebuilding the design requires an active AMD [100G Multirate Ethernet MAC IP license](https://www.amd.com/en/products/adaptive-socs-and-fpgas/intellectual-property/mrmac.html).
- **NVIDIA HSB IP:** Apache License, Version 2.0, © NVIDIA (`third_party/hsb/LICENSE`).
- **Backline Sources:** All other files in this folder are licensed under the Apache License,
  Version 2.0 (`LICENSE`).

### HSB IP

The HSB IP is vendored unmodified. `third_party/hsb/nv_hsb_ip/` is NVIDIA's `EA_CUDAQ_10_26`
branch at `a4b2059` (IP revision 0x2609), byte for byte. `third_party/hsb/UPSTREAM.md` shows how to
check this and how to re-vendor it:

```bash
cd third_party/hsb/nv_hsb_ip
shasum -a 256 -c ../MANIFEST.sha256
```

Everything VPK120-specific lives outside that directory:

- `hw/rtl/hsb_cfg/HOLOLINK_def.svh` is the build configuration. It sets two host ports, one sensor
  RX/TX pair and a 250 MHz hif clock; the rest is NVIDIA's RFSoC configuration.
- `hw/rtl/top/backline_hsb_top.sv` is the board top that instantiates HSB.

### Folder Layout
```shell
├── common
│   └── include
├── host
│   ├── hsb-echo
│   └── hsb-gpu-echo
├── hw
│   ├── bd
│   ├── constraints
│   ├── rtl
│   │   ├── demo
│   │   ├── hsb_cfg
│   │   ├── hwhs
│   │   └── top
│   ├── scripts
│   └── sim
├── scripts
├── sw
│   ├── apps
│   │   ├── hsbctl
│   │   └── mrmac-up
│   ├── lib
│   │   └── hwhs
│   ├── petalinux
│   │   └── meta-user
│   │       ├── recipes-apps
│   │       │   ├── hwhs-tools
│   │       │   └── vpk-init
│   │       ├── recipes-bsp
│   │       │   └── device-tree
│   │       └── recipes-kernel
│   │           └── linux
│   └── tests
└── third_party
    └── hsb
        └── nv_hsb_ip
```

| Directory | Description |
| --- | --- |
| /third_party/hsb | HSB IP (vendored, unmodified), its license, manifest and upstream notes |
| /hw/rtl/hsb_cfg | `HOLOLINK_def.svh`: HSB build configuration for the VPK120 |
| /hw/rtl/hwhs | hwhs-hsb: registers, round engine, slot/reply/trace RAMs, ECB mailbox |
| /hw/rtl/demo | `transport_demo.v`, the hw-loop syndrome pacer (unchanged from the ERNIC build) |
| /hw/rtl/top | `backline_hsb_top.sv`: AMD's MRMAC example design + platform BD + HSB + hwhs |
| /hw/bd | `platform.tcl` (block design). `amd_procs.tcl` takes the CIPS, NoC and MRMAC procs from AMD's Tcl at build time |
| /hw/constraints | Clock groups; HSB's RAM style |
| /hw/scripts | `create_project.tcl`, `build.tcl` (Vivado 2024.2, batch) |
| /hw/sim | Verilator lint and system simulation; Tcl dry run of the project scripts |
| /common/include | Headers shared by the board and the host: address and register maps, HSB registers, session handshake |
| /sw/lib/hwhs | The driver: UIO (or `/dev/mem`) mapping, ECB access, HSB programming, rounds, trace |
| /sw/apps | `hsbctl` (controller CLI), `mrmac-up` (100G link bring-up from user space) |
| /sw/petalinux | Image build script and `meta-user`: device tree, UIO kernel config, recipes, boot service |
| /sw/tests | Driver tests that run on any host |
| /host/hsb-echo | The coprocessor test peer (ibverbs) |
| /host/hsb-gpu-echo | The same peer on the GPU, the way catalyst's GPU coprocessor works (HIP + ibverbs) |
| /scripts | `unpack-amd.sh`: takes the AMD files the build needs from AMD's download |

The address map is in the header of `hw/bd/platform.tcl`. The hwhs register map is in
`common/include/hwhs_regs.h`, which mirrors `hw/rtl/hwhs/hwhs_regs.v`.

## Prerequisites

### Common

- **Hardware:** AMD Versal VPK120 board (rev B), a QSFP-DD 100G link to the coprocessor's
  ConnectX, Micro-USB or USB-C cable for JTAG/UART, and a 1 GbE management Ethernet cable.
- **Software:** AMD Vivado™ Design Suite 2024.2 installed on the host PC. The `xsdb` executable
  must be available after sourcing the Vivado environment.
- **Coprocessor host:** Linux with a ConnectX NIC (RoCE v2) and rdma-core. The GPU echo
  (`hsb-gpu-echo`) also needs ROCm.

### Build

- A Linux build host with AMD Vivado 2024.2 (Versal Premium support) and AMD PetaLinux 2024.2.
- AMD **ERNIC VPK120 MRMAC reference design**, v1.3
  (release `ERNIC-2024_2_VPK120_RC6`), downloaded as
  `reference design VPK120_mrmac_rev_1.3.zip`.
  - The build takes CIPS, the NoC/LPDDR4, the MRMAC core-support BD, the MRMAC example design
    RTL, the clock wizard and the XDCs from it.
  - The VPK120 XSCT 2024.2 BSP ships in the same zip, and so does the VPK120 board device tree
    (inside the ERNIC software package).

### Checks without Vivado (optional)

- Verilator 5.048 or newer, and tclsh.

## Build the Images

Unpack the AMD files the build needs. `WITH_BSP=1` also extracts the PetaLinux BSP:

```bash
make amd AMD="<dir>/reference design VPK120_mrmac_rev_1.3.zip" WITH_BSP=1   # -> build/amd
```

Build the hardware design:

```bash
source /path/to/Vivado/2024.2/settings64.sh
make hw JOBS=<N>            # -> build/vivado/backline_hsb/backline_hsb.xsa (+ .pdi, reports/)
```

**`make hw` always creates the Vivado project from scratch.** That is when the build marker is
stamped: `BUILD_ID` is yymmddHH read as hex, e.g. `0x26102115`, and `create_project.tcl` prints it.
Before you trust a board result, check that `hsbctl status` shows the same `BUILD_ID`.

Build the PetaLinux image:

```bash
source /path/to/petalinux/settings.sh
make petalinux              # -> build/petalinux/backline-hsb/images/linux, BOOT.BIN, jtag-boot.tcl
```

To use a BSP from another location, pass `make petalinux BSP=/path/to/xilinx-vpk120-xsct-v2024.2-final.bsp`.

Other build options:

- To rerun only synthesis and implementation on an existing project:
  `vivado -mode batch -source hw/scripts/build.tcl -tclargs -xpr build/vivado/backline_hsb/backline_hsb.xpr`.
  The marker does not change in that case.
- `make project` only creates the Vivado project, e.g. to open it in the GUI.
- `HIF_MHZ=<n>` (default 250) sets HSB's host/sensor clock and hwhs's clock together. `build.tcl`
  prints a critical warning when timing is not met; lowering `HIF_MHZ` is the first thing to try.
- `make apps CC=<aarch64 cc>` builds `hsbctl` and `mrmac-up` outside PetaLinux. The image build
  compiles the same sources through the `hwhs-tools` recipe.

## Program the Board

Set the board boot-mode switches to **JTAG mode** and power-cycle the board. With the Vivado
environment sourced, run:

```bash
make boot                   # runs xsdb jtag-boot.tcl in build/petalinux/backline-hsb
```

If the JTAG cable permissions are not configured in the host's udev rules, `xsdb` may need to be
run with `sudo`.

After the board boots, continue with [Serial Console & Login](#serial-console--login).

## Serial Console & Login

1. Connect to the board's serial port using a terminal emulator such as Minicom, Tera Term, or
   Picocom.
2. Log in to PetaLinux:
   * **Default Username:** `root`
   * **Default Password:** `root`

   The image also has the `petalinux` / `2024` login used by the ERNIC build.

Change the default passwords when the board is placed on a shared or routed network.

At boot, the `vpk-init` service binds the PL windows to UIO, configures the management interface,
runs `mrmac-up` and prints `hsbctl status`.

## Network Setup

The following addresses match the Backline reference setup:

| Interface | Address |
| --- | --- |
| VPK120 `end0` (management, set by `vpk-init`) | 192.168.3.15/24 |
| HSB host port 0 (100G), after reset | 192.168.0.2, MAC 02:93:c6:48:4d:8e |
| Coprocessor RoCE netdev | A free address in 192.168.0.0/24, e.g. 192.168.0.1. Not .2 (HSB), and not .3 or .254 (the mailbox uses them inside the FPGA) |
| Coprocessor management | A free address in 192.168.3.0/24 |

The board needs no manual network commands: `vpk-init` configures `end0`, and HSB host port 0 is
in the FPGA, not a Linux interface. No static ARP entry is needed either, because HSB answers ARP
and ICMP itself.

On the coprocessor host, replace `<roce netdev>` and `<mgmt netdev>` with your interface names:

```bash
sudo ip addr add 192.168.0.1/24 dev <roce netdev>
sudo ip link set <roce netdev> up
sudo ip addr add 192.168.3.<N>/24 dev <mgmt netdev>
sudo ip link set <mgmt netdev> up
```

## Bring-up Checks

Check, in order:

1. **`hsbctl status`** on the board:
   - `BUILD_ID` matches the build.
   - `clk_locked=1 hsb_init_done=1 hsb_in_reset=0`.
   - `hif clock 250000000 Hz`.
   - `HSB IP version 0x2609`. This value is read over ECB through the mailbox, so it proves the
     A72 → HSB configuration path.
2. **`mrmac-up -s`** on the board shows `status 1 aligned 1` and `BLKLCK 0xfffff`. If not, check
   the cable and the far end, then run `mrmac-up` again.
3. **On the coprocessor host:**
   - `ping 192.168.0.2` gets replies.
   - `tcpdump -ni <roce netdev> udp port 12267` shows HSB's BOOTP announcement about once a
     second.

## Run a Session

Start the echo peer on the coprocessor host. It serves one session per start and exits when the
controller disconnects:

```bash
# CPU echo
make host
host/hsb-echo/hsb-echo -d mlx5_1 -c 4          # -c: the core rt_tune.sh isolates (4,36); -g: the RoCE v2 GID of 192.168.0.x (default 3)

# or the GPU echo: requests land in GPU memory, a persistent kernel echoes them,
# the CPU thread on -c posts the replies
make host-gpu
host/hsb-gpu-echo/hsb-gpu-echo -d mlx5_1 -c 4   # -G: the HIP device (default 0)
```

Then run the rounds from the board:

```bash
hsbctl run -H <coprocessor mgmt ip> -n 10000                          # sw-loop: the A72 runs each round
hsbctl run -H <coprocessor mgmt ip> -m hw -n 100000 -P 10 -o rtt.csv  # hw-loop: transport_demo, a round every 10 us
```

`hsbctl run` does the following:

- runs the handshake (TCP 7760, over the management network);
- resets HSB and programs its data plane (`hwhs_hsb_configure` in `sw/lib/hwhs/hwhs.c`);
- runs the rounds;
- prints `RTT (ns) ... min p50 p99 p99.9 max`.

It exits non-zero on any timeout or reply mismatch.

Other commands: `hsbctl reg <off> [val]`, `hsbctl ecb-rd|ecb-wr <addr> [val]` (any HSB register),
`hsbctl hsb-reset`, `hsbctl trace <file>`.

## Checks Without Vivado

```bash
make check      # lint + sim + dryrun + test; dryrun needs `make amd` first
```

- **`make dryrun`** runs `create_project.tcl` in tclsh with the Vivado commands mocked. It checks
  that every block-design cell, port, pin and address segment the scripts reference exists.
- **`make test`** checks that the driver builds the ECB frame byte for byte the way the simulated
  HSB accepted it.

Versal, and Vivado, are trademarks of Advanced Micro Devices, Inc.
