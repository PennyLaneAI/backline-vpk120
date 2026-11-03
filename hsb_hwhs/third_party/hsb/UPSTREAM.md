# NVIDIA Holoscan Sensor Bridge FPGA IP

| | |
|---|---|
| Upstream | https://github.com/nvidia-holoscan/holoscan-sensor-bridge |
| Branch | `EA_CUDAQ_10_26` (the NVQLink / CUDA-Q early-access branch) |
| Commit | `a4b2059b59f3d7beae3881b172df9552382e9c61` (2026-10-16, "Publish branch for NVQlink") |
| Path | `fpga/nv_hsb_ip/` -> `nv_hsb_ip/` + the top-level `LICENSE` |
| IP revision | `HOLOLINK_REV = 16'h2609` (`nv_hsb_ip/top/HOLOLINK_top.sv`) |
| License | Apache-2.0 (`LICENSE`) |

For VPK120-specific lives outside this directory:
- the build configuration (`HOLOLINK_def.svh`) is `hw/rtl/hsb_cfg/HOLOLINK_def.svh`.
- the board top that instantiates `HOLOLINK_top` is `hw/rtl/top/backline_hsb_top.sv`.

## Check that the copy is unmodified

```bash
cd third_party/hsb/nv_hsb_ip
shasum -a 256 -c ../MANIFEST.sha256
```

## Re-vendor from a local clone

```bash
cd <path/to/your/holoscan-sensor-bridge>
mkdir -p /tmp/hsb && git archive --format=tar a4b2059 fpga/nv_hsb_ip LICENSE | tar -x -C /tmp/hsb
```

Then replace `nv_hsb_ip/` and `LICENSE` with `/tmp/hsb/fpga/nv_hsb_ip` and `/tmp/hsb/LICENSE`,
and regenerate `MANIFEST.sha256` the same way it was made:

```bash
cd nv_hsb_ip && find . -type f | LC_ALL=C sort | xargs shasum -a 256 > ../MANIFEST.sha256
```
