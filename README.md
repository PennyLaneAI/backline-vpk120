# backline-vpk120

Backline's FPGA controller on the AMD Versal™ Premium VPK120, in three builds. Each folder is a
complete build with its own README, and the board runs one of them at a time.

| Folder | Network IP on the FPGA | Round engine | Catalyst backend |
|---|---|---|---|
| [`ernic_hwhs/`](ernic_hwhs/README.md) | AMD ERNIC | hwhs | `hwhs`, or `fpga_verbs` for the CPU-driven round |
| [`hsb_hwhs/`](hsb_hwhs/README.md) | NVIDIA Holoscan Sensor Bridge (HSB) | hwhs-hsb | `hsb_hwhs` |
| [`balboa_hwhs/`](balboa_hwhs/README.md) | ETH Zurich's BALBOA RoCE v2 stack | hwhs-balboa | `balboa_hwhs` |

In each, the controller sends each syndrome to a coprocessor over 100G RoCE v2 and times the
round trip. The difference is the IP that carries it: ERNIC is AMD's RDMA NIC IP, HSB is NVIDIA's
open-source sensor bridge IP, and BALBOA is ETH Zurich's open-source RoCE v2 stack.

All three builds start from AMD's **ERNIC VPK120 MRMAC reference design, rev 1.3**
(`reference design VPK120_mrmac_rev_1.3.zip`), which you download from AMD. No folder
redistributes it.

## Licensing

Each folder is licensed on its own terms; see its README.

- **`ernic_hwhs/`**:
  - The HDL patch and scripts in `hdl/` are Apache-2.0.
  - The software in `code/` is a fork of AMD's ERNIC software, under the AMD PetaLinux EULA and
    the open-source licenses of its parts.
  - Rebuilding needs AMD's ERNIC and 100G Multirate Ethernet MAC IP licenses.
- **`hsb_hwhs/`**:
  - Our files are Apache-2.0 (`hsb_hwhs/LICENSE`).
  - HSB is Apache-2.0, © NVIDIA, vendored unmodified in `hsb_hwhs/third_party/hsb`.
  - No AMD file is in the folder: `make amd` unpacks the parts of AMD's download that the build
    reads, the board device tree included, into `build/`.
  - Rebuilding needs AMD's 100G Multirate Ethernet MAC IP license. The design does not use the
    ERNIC IP, so it needs no ERNIC IP license, but it does need AMD's ERNIC reference design,
    which AMD provides on its own terms.
- **`balboa_hwhs/`**:
  - Our files are Apache-2.0 (`balboa_hwhs/LICENSE`).
  - BALBOA is vendored unmodified in `balboa_hwhs/third_party/balboa`: Coyote's RTL is MIT, © ETH
    Zurich, and the network stack's HLS cores are BSD-3-Clause, © ETH Zurich (three of them also
    © Xilinx, Inc.).
  - No AMD file is in the folder: `make amd` unpacks the parts of AMD's download that the build
    reads, the board device tree included, into `build/`.
  - Rebuilding needs AMD's 100G Multirate Ethernet MAC IP license. The design does not use the
    ERNIC IP, so it needs no ERNIC IP license, but it does need AMD's ERNIC reference design,
    which AMD provides on its own terms.

Versal, and Vivado, are trademarks of Advanced Micro Devices, Inc.
