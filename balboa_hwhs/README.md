# VPK120 Board Setup and Configuration Guide (BALBOA)

## Overview
This README provides instructions and scripts for building ETH Zurich's BALBOA RoCE v2 stack for
the AMD Versal™ Premium Evaluation Kit (VPK120), programming the board, and bringing up its link
to the coprocessor for use with backline.

The design places Backline's hardware-handshake controller (hwhs-balboa) in front of BALBOA. The
images are built from source:

1. [Build the images](#build-the-images) from AMD's reference design and this folder.
2. [Program the board](#program-the-board) over JTAG.
3. Continue with [Serial Console & Login](#serial-console--login),
   [Network Setup](#network-setup), [Bring-up Checks](#bring-up-checks) and
   [Run a Session](#run-a-session).

### How a Round Works

One round: the A72 (or `transport_demo`, in hw-loop mode) puts a 64 B request in the slot RAM and
starts the engine. hwhs stamps a sequence number into the request and posts it to BALBOA, which
sends it as an RC RDMA WRITE on QP 0 into the next slot of the coprocessor's ring. The coprocessor
answers with an RC RDMA WRITE of 16 B into the FPGA. BALBOA hands that payload to hwhs, and hwhs
matches the sequence number and records the reply and the RTT. BALBOA ACKs the coprocessor's
WRITEs and retransmits its own on loss.

The A72 sets BALBOA's address, resolves the coprocessor's MAC and loads the QP through hwhs
registers (`NET_CTRL`, `QP_CTX`, `CONN_CTX`).

### Licensing and Compliance

Individual subsystems within this folder are licensed under their respective directories.

- **AMD Vivado License:** Must include support for Versal Premium devices (VPK120).
- **Reference Design Sources:** The AMD ERNIC MRMAC reference design remains the intellectual
  property of Advanced Micro Devices, Inc. Users must download it directly from AMD. `make amd`
  unpacks the parts the build reads into `build/amd`:
  - for Vivado: the CIPS, NoC and MRMAC Tcl, the MRMAC example design RTL, the clock wizard and the XDCs;
  - for PetaLinux: the BSP and the VPK120 rev B board device tree (`versal-vpk120-revB.dtsi`,
    GPL-2.0, © Xilinx). `sw/petalinux/build.sh` copies the dtsi into the image's device-tree
    recipe.
- **AMD Versal 100G Multirate Ethernet MAC IP License:** Rebuilding the design requires an active AMD [100G Multirate Ethernet MAC IP license](https://www.amd.com/en/products/adaptive-socs-and-fpgas/intellectual-property/mrmac.html).
- **BALBOA:** Coyote's RTL is MIT, © ETH Zurich (`third_party/balboa/coyote/LICENSE.md`). The HLS
  cores from fpga-network-stack are BSD-3-Clause, © ETH Zurich; `ip_handler`, `mac_ip_encode` and
  `arp_server_subnet` also name Xilinx, Inc. as a copyright holder. See
  `third_party/balboa/UPSTREAM.md`.
- **Backline Sources:** All other files in this folder are licensed under the Apache License,
  Version 2.0 (`LICENSE`).

### BALBOA

The BALBOA sources are vendored unmodified. `third_party/balboa/` holds the part of Coyote
(`15e5f59`, the commit the OSDI '26 artifact `BALBOA_AE` pins) and of fpga-network-stack
(`1197a9d`, Coyote's submodule) that the build compiles, byte for byte.
`third_party/balboa/UPSTREAM.md` shows how to check this and how to re-vendor it:

```bash
cd third_party/balboa
shasum -a 256 -c MANIFEST.sha256
```

Everything VPK120-specific lives outside that directory:

- `hw/rtl/balboa_cfg/lynx_pkg.sv` is Coyote's `lynxTypes` package rendered for this build: RDMA
  and statistics on, the board's IP and MAC as the defaults.
- `hw/rtl/balboa_cfg/icmp_server_ip.sv` and `vio_ip.sv` take the place of an HLS core and a
  Vivado debug core that `network_stack.sv` instantiates. `axis_interconnect_512.sv` takes the
  place of its two AXI4-Stream Interconnect cores, which Vivado does not offer for Versal.
- `hw/scripts/build_hls.sh` runs Vitis HLS on BALBOA's four HLS cores the way Coyote's CMake does.
- `hw/scripts/balboa_ips.tcl` creates the Vivado IP cores the vendored RTL instantiates, with
  Coyote's configurations.
- `hw/rtl/top/balboa_net.sv` wraps `network_stack.sv`; `hw/rtl/top/backline_roce_top.sv` is the
  board top.

BALBOA's QP is QP 0. Coyote's `rdma_flow.sv` reads QP 0's flow-control and retransmission
pointers whatever the QP, so QP 0 is the one it tracks correctly (`HWHS_FPGA_QPN` in
`sw/lib/hwhs/hwhs.h`).

### Folder Layout
```shell
├── common
│   └── include
├── host
│   ├── balboa-echo
│   ├── balboa-gpu-echo
│   ├── balboa-standin
│   └── common
├── hw
│   ├── bd
│   ├── hls
│   │   └── csim
│   ├── rtl
│   │   ├── balboa_cfg
│   │   ├── demo
│   │   ├── hwhs
│   │   └── top
│   ├── scripts
│   └── sim
├── scripts
├── sw
│   ├── apps
│   │   ├── balboactl
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
    └── balboa
        ├── coyote
        └── fpga-network-stack
```

| Directory | Description |
| --- | --- |
| /third_party/balboa | BALBOA's RTL and HLS sources (vendored, unmodified), manifest and upstream notes |
| /hw/rtl/balboa_cfg | `lynx_pkg.sv` (BALBOA build configuration) and stand-ins for cores `network_stack.sv` instantiates |
| /hw/rtl/hwhs | hwhs-balboa: registers, round engine, slot/reply/trace RAMs, retransmission buffer |
| /hw/rtl/demo | `transport_demo.v`, the hw-loop syndrome pacer (unchanged from the ERNIC build) |
| /hw/rtl/top | `backline_roce_top.sv`: AMD's MRMAC example design + platform BD + BALBOA + hwhs; `balboa_net.sv`; `eth_pad.v` |
| /hw/bd | `platform.tcl` (block design). `amd_procs.tcl` takes the CIPS, NoC and MRMAC procs from AMD's Tcl at build time |
| /hw/scripts | `build_hls.sh` (Vitis HLS 2024.2), `balboa_ips.tcl`, `create_project.tcl`, `build.tcl` (Vivado 2024.2, batch) |
| /hw/sim | Verilator lint and simulation; Tcl dry run of the project scripts |
| /hw/hls/csim | BALBOA's RoCE HLS core as plain C++ against the driver's QP setup |
| /common/include | Headers shared by the board and the host: address and register maps, session handshake |
| /sw/lib/hwhs | The driver: UIO (or `/dev/mem`) mapping, BALBOA setup, rounds, trace |
| /sw/apps | `balboactl` (controller CLI), `mrmac-up` (100G link bring-up from user space) |
| /sw/petalinux | Image build script and `meta-user`: device tree, UIO kernel config, recipes, boot service |
| /sw/tests | Driver tests that run on any host |
| /host/balboa-echo | The coprocessor test peer (ibverbs) |
| /host/balboa-gpu-echo | The same peer on the GPU, the way catalyst's GPU coprocessor works (HIP + ibverbs) |
| /host/balboa-standin | The board's side of a session in software, to test the peers without the board |
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
  (`balboa-gpu-echo`) also needs ROCm.

### Build

- A Linux build host with AMD Vivado 2024.2 (Versal Premium support), Vitis HLS 2024.2 (installed
  with Vitis 2024.2) and AMD PetaLinux 2024.2.
- AMD **ERNIC VPK120 MRMAC reference design**, v1.3
  (release `ERNIC-2024_2_VPK120_RC6`), downloaded as
  `reference design VPK120_mrmac_rev_1.3.zip`.
  - The build takes CIPS, the NoC/LPDDR4, the MRMAC core-support BD, the MRMAC example design
    RTL, the clock wizard and the XDCs from it.
  - The VPK120 XSCT 2024.2 BSP ships in the same zip, and so does the VPK120 board device tree
    (inside the ERNIC software package).

### Checks without Vivado (optional)

- Verilator 5.0 or newer, tclsh, a C/C++ compiler and git. On macOS, `make csim` needs GCC
  (`brew install gcc`).

## Build the Images

Unpack the AMD files the build needs. `WITH_BSP=1` also extracts the PetaLinux BSP:

```bash
make amd AMD="<dir>/reference design VPK120_mrmac_rev_1.3.zip" WITH_BSP=1   # -> build/amd
```

Build the hardware design:

```bash
source /path/to/Vitis/2024.2/settings64.sh      # Vivado and Vitis HLS
make hw JOBS=<N>            # -> build/vivado/backline_roce/backline_roce.xsa (+ .pdi, reports/)
```

`make hw` first runs Vitis HLS on any of BALBOA's HLS cores that is not in `build/hls/iprepo` yet,
then creates the Vivado project from scratch and builds it. `make hls` reruns all four cores.

**The build marker is stamped when the project is created.** `BUILD_ID` is yymmddHH read as hex,
e.g. `0x26110416`, and `create_project.tcl` prints it. Before you trust a board result, check that
`balboactl status` shows the same `BUILD_ID`.

Build the PetaLinux image:

```bash
source /path/to/petalinux/settings.sh
make petalinux              # -> build/petalinux/backline-roce/images/linux, BOOT.BIN, jtag-boot.tcl
```

Downloads and the sstate cache stay in `build/petalinux` between builds. Running `make petalinux`
again, for a new XSA or after a failed download, reuses them instead of fetching and compiling
everything again.

To use a BSP from another location, pass `make petalinux BSP=/path/to/xilinx-vpk120-xsct-v2024.2-final.bsp`.

Other build options:

- To rerun only synthesis and implementation on an existing project:
  `vivado -mode batch -source hw/scripts/build.tcl -tclargs -xpr build/vivado/backline_roce/backline_roce.xpr`.
  The marker does not change in that case.
- `make project` only creates the Vivado project, e.g. to open it in the GUI.
- `NET_MHZ=<n>` (default 250) sets the clock of BALBOA, hwhs and `transport_demo`. Each HLS core
  records the clock it was synthesized for, and `create_project.tcl` stops if that is not
  `NET_MHZ`; run `make hls NET_MHZ=<n>` after changing it. `build.tcl` prints a critical warning
  when timing is not met.
- `make apps CC=<aarch64 cc>` builds `balboactl` and `mrmac-up` outside PetaLinux. The image build
  compiles the same sources through the `hwhs-tools` recipe.

## Program the Board

Set the board boot-mode switches to **JTAG mode** and power-cycle the board. With the Vivado
environment sourced, run:

```bash
make boot                   # runs xsdb jtag-boot.tcl in build/petalinux/backline-roce
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

   The image also has the `petalinux` / `2024` login used by the ERNIC and HSB builds.

Change the default passwords when the board is placed on a shared or routed network.

At boot, the `vpk-init` service binds the PL windows to UIO, configures the management interface,
runs `mrmac-up` and prints `balboactl status`.

## Network Setup

The following addresses match the Backline reference setup, and the HSB build's:

| Interface | Address |
| --- | --- |
| VPK120 `end0` (management, set by `vpk-init`) | 192.168.3.15/24 |
| BALBOA (100G), after reset | 192.168.0.2, MAC 02:93:c6:48:4d:8e |
| Coprocessor RoCE netdev | A free address in 192.168.0.0/24, e.g. 192.168.0.1 |
| Coprocessor management | A free address in 192.168.3.0/24 |

The board needs no manual network commands: `vpk-init` configures `end0`, and BALBOA is in the
FPGA, not a Linux interface. No static ARP entry is needed either, because BALBOA answers ARP and
ping itself. A coprocessor host set up for the HSB build needs no change.

On the coprocessor host, replace `<roce netdev>` and `<mgmt netdev>` with your interface names:

```bash
sudo ip addr add 192.168.0.1/24 dev <roce netdev>
sudo ip link set <roce netdev> up
sudo ip addr add 192.168.3.<N>/24 dev <mgmt netdev>
sudo ip link set <mgmt netdev> up
```

## Bring-up Checks

Check, in order:

1. **`balboactl status`** on the board:
   - `BUILD_ID` matches the build.
   - `clk_locked=1 stack_up=1 stack_in_reset=0`.
   - `net clock 250000000 Hz`.
   - `address 192.168.0.2 02:93:c6:48:4d:8e`.
2. **`mrmac-up -s`** on the board shows `status 1 aligned 1` and `BLKLCK 0xfffff`. If not, check
   the cable and the far end, then run `mrmac-up` again.
3. **`ping 192.168.0.2`** on the coprocessor host gets replies, and `balboactl stats` on the board
   counts them as ICMP rx/tx.
4. **`balboactl arp 192.168.0.1`** on the board prints `ARP reply received`.

## Run a Session

Start the echo peer on the coprocessor host. It serves one session per start and exits when the
controller disconnects:

```bash
# CPU echo
make host
host/balboa-echo/balboa-echo -d mlx5_0 -g 5 -c 18     # -g: the RoCE v2 GID of 192.168.0.1 (default 3); -c: an isolated core

# or the GPU echo: requests land in GPU memory, a persistent kernel echoes them,
# the CPU thread on -c posts the replies
make host-gpu
host/balboa-gpu-echo/balboa-gpu-echo -d mlx5_0 -g 5 -c 18   # -G: the HIP device (default 0)
```

`-r` adds SCHED_FIFO and `/dev/cpu_dma_latency` = 0, as catalyst's `rt=1`; it needs root. For a
ROCm that is not on `PATH`, build with `make host-gpu HIPCC=/opt/rocm-<version>/bin/hipcc` and run
with `LD_LIBRARY_PATH=/opt/rocm-<version>/lib`.

Then run the rounds from the board:

```bash
balboactl run -H <coprocessor mgmt ip> -n 10000                          # sw-loop: the A72 runs each round
balboactl run -H <coprocessor mgmt ip> -m hw -n 100000 -P 10 -o rtt.csv  # hw-loop: transport_demo, a round every 10 us
```

`balboactl run` does the following:

- runs the handshake (TCP 7760, over the management network);
- resets BALBOA, sets its address, resolves the coprocessor's MAC and loads the QP;
- runs the rounds;
- prints `RTT (ns) ... min p50 p99 p99.9 max` and BALBOA's counters.

It exits non-zero on any timeout or reply mismatch.

Other commands: `balboactl stats`, `balboactl reg <off> [val]`, `balboactl stack-reset`,
`balboactl arp <ip>`, `balboactl trace <file>`.

To test the peers without the board, `balboa-standin` runs the board's side of a session in
software. It can share the peer's NIC:

```bash
make host-standin
host/balboa-standin/balboa-standin -d mlx5_0 -g 5 -H 127.0.0.1 -n 100000
```

## Checks Without Vivado

```bash
make check      # lint + sim + dryrun + test + csim; dryrun needs `make amd` first
                # on macOS: make check CSIM_CXX=g++-15
```

- **`make sim`** runs the hwhs engine alone, hwhs with BALBOA's `roce_stack` and a model of the
  RoCE HLS core, and the frame padding, ping responder and TX stream merges.
- **`make dryrun`** runs `create_project.tcl` in tclsh with the Vivado commands mocked. It checks
  that every block-design cell, port, pin and address segment the scripts reference exists, and
  that every IP core the RTL instantiates is created.
- **`make test`** checks the QP and connection words the driver writes into BALBOA.
- **`make csim`** compiles BALBOA's RoCE HLS core as plain C++ and runs it against the driver's QP
  setup: a request WRITE goes out with the right headers, the coprocessor's reply WRITE comes in
  and is ACKed, and a WRITE with a wrong PSN is dropped. It uses Vitis HLS's headers when
  installed, else it clones Xilinx's open-source `HLS_arbitrary_Precision_Types` (Apache-2.0)
  into `build/csim`.

Versal, and Vivado, are trademarks of Advanced Micro Devices, Inc.
