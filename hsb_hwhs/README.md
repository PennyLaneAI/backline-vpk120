# hsb_hwhs

Backline's FPGA controller on the VPK120, with NVIDIA's Holoscan Sensor Bridge (HSB) IP as the
network IP.

## Layout

| Path | Contents |
|---|---|
| `third_party/hsb/` | NVIDIA's HSB IP with its license |
| `hw/rtl/hsb_cfg/HOLOLINK_def.svh` | HSB build configuration for the VPK120 |
| `hw/rtl/top/backline_hsb_top.sv` | board top that instantiates HSB |

## Licensing

- `hw/` is Apache-2.0; see `LICENSE`. `hw/rtl/hsb_cfg/HOLOLINK_def.svh` is adapted from NVIDIA's HSB
   configuration.
- `third_party/hsb/` is NVIDIA's HSB IP under Apache-2.0; see `third_party/hsb/LICENSE`.

NVIDIA and Holoscan are trademarks of NVIDIA Corporation.
