# BALBOA (RoCE CREED)

BALBOA is ETH Zurich's 100G RoCE v2 stack, published with the OSDI '26 paper "RoCE CREED:
Service-Enhanced RDMA Offload Engine for Data Center SmartNICs". The artifact repository pins
Coyote v2, and Coyote pins the network stack as a submodule. This directory holds the part of
each that the backline-roce build compiles, unmodified.

| | Upstream | Branch | Commit |
|---|---|---|---|
| Artifact | https://github.com/fpgasystems/BALBOA_AE | `main` | `0d30401432a4e4cf0fe4aa1cf6ba94ab2db4c515` (2026-05-14) |
| `coyote/` | https://github.com/fpgasystems/Coyote | `artefacts/balboa-ae` | `15e5f5905051c3800d81720ecd5f53d7ab44116f` (2026-04-28), the artifact's `Coyote` submodule |
| `fpga-network-stack/` | https://github.com/fpgasystems/fpga-network-stack | `coyote-TCP-RDMA` | `1197a9dfa172b34c50ad5ee0a418a53fdddff146` (2026-03-27), Coyote's `hw/services/network` submodule |

`MANIFEST.sha256` lists every file. In short:

- `coyote/`: `LICENSE.md`; `hw/hdl/network/stack/network_stack.sv`; `hw/hdl/network/rdma/roce_stack.sv`
  and the modules under it; the queues, registers and RAM those use; `hw/hdl/pkg/`;
  `hw/templates/common/lynx_pkg_tmplt.txt`.
- `fpga-network-stack/`: the HLS cores `rocev2`, `ip_handler`, `mac_ip_encode` and
  `arp_server_subnet` (sources, config templates, `make.tcl.in`), and what they include or compile
  with: `axi_utils`, `packet.hpp`, `ethernet/`, `ipv4/`, `ipv6/`, `udp/`, `crc/`,
  `ib_transport_protocol/`.

## Licenses

- `coyote/` is MIT, © ETH Zurich (`coyote/LICENSE.md`). The artifact's README says the CREED code
  inside Coyote is under the same license.
- `fpga-network-stack/` has no license file. Each `.cpp` and `.hpp` carries a BSD-3-Clause header,
  © 2018-2019 Systems Group, ETH Zurich. The `ip_handler`, `mac_ip_encode` and
  `arp_server_subnet` sources also name Xilinx, Inc. (2016) as a copyright holder, under the same
  BSD-3-Clause terms. The config templates and `make.tcl.in` files have no header.

## Not taken

- `icmp_server` (© Xilinx, BSD-3-Clause). `hw/rtl/balboa_cfg/icmp_server_ip.sv` takes its place.
- `ethernet_frame_padding_512`. `hw/rtl/top/eth_pad.v` pads short frames instead.
- The rest of Coyote (the shell, the other network modules, memory, software) and the network
  stack's other HLS cores, TCP among them.
- Testbenches, CMake files and test data.
- The artifact's experiments and its `balboa_dpi` submodule.

## Where the build connects

- `hw/rtl/balboa_cfg/lynx_pkg.sv` is Coyote's `lynxTypes` package, rendered from
  `lynx_pkg_tmplt.txt` for this build. Its header lists the settings and the edits.
- `hw/rtl/balboa_cfg/vio_ip.sv` is an empty stand-in for the VIO core `network_stack.sv`
  instantiates.
- `hw/rtl/balboa_cfg/axis_interconnect_512.sv` takes the place of the two AXI4-Stream
  Interconnect cores `network_stack.sv` merges its TX streams with; Vivado does not offer that
  core for Versal.
- `hw/scripts/build_hls.sh` fills in `make.tcl.in` and the config templates the way Coyote's CMake
  does, and runs Vitis HLS.
- `hw/scripts/balboa_ips.tcl` creates the Vivado IP cores the vendored RTL instantiates by name,
  with Coyote's configurations.
- `hw/rtl/top/balboa_net.sv` wraps `network_stack.sv` for the board top.

## Check that the copy is unmodified

```bash
cd third_party/balboa
shasum -a 256 -c MANIFEST.sha256
```

## Re-vendor from local clones

The manifest is the file list:

```bash
cd third_party/balboa
awk '{print $2}' MANIFEST.sha256 | sed -n 's|^coyote/||p' | \
    xargs git -C <path/to/Coyote> archive --format=tar 15e5f5905051c3800d81720ecd5f53d7ab44116f | tar -x -C coyote
awk '{print $2}' MANIFEST.sha256 | sed -n 's|^fpga-network-stack/||p' | \
    xargs git -C <path/to/fpga-network-stack> archive --format=tar 1197a9dfa172b34c50ad5ee0a418a53fdddff146 | tar -x -C fpga-network-stack
```

After adding or removing files, regenerate the manifest:

```bash
find coyote fpga-network-stack -type f | LC_ALL=C sort | xargs shasum -a 256 > MANIFEST.sha256
```
