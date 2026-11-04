# backline-vpk120

Backline's FPGA controller on the AMD Versal™ Premium VPK120. Each folder is a complete build
with its own README.

| Folder | Network IP on the FPGA | Round engine | Catalyst backend |
|---|---|---|---|
| [`ernic_hwhs/`](ernic_hwhs/README.md) | AMD ERNIC | hwhs | `hwhs`, or `fpga_verbs` for the CPU-driven round |

The controller sends each syndrome to a coprocessor over 100G RoCE v2 and times the round trip.
ERNIC is AMD's RDMA NIC IP, which carries it.

The build starts from AMD's **ERNIC VPK120 MRMAC reference design, rev 1.3**
(`reference design VPK120_mrmac_rev_1.3.zip`), which you download from AMD. This repository does
not redistribute it.

## Licensing

Each folder is licensed on its own terms; see its README.

- **`ernic_hwhs/`**:
  - The HDL patch and scripts in `hdl/` are Apache-2.0.
  - The software in `code/` is a fork of AMD's ERNIC software, under the AMD PetaLinux EULA and
    the open-source licenses of its parts.
  - Rebuilding needs AMD's ERNIC and 100G Multirate Ethernet MAC IP licenses.

Versal, and Vivado, are trademarks of Advanced Micro Devices, Inc.
