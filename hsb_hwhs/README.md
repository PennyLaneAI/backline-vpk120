# hsb_hwhs

Backline's FPGA controller on the VPK120, with NVIDIA's Holoscan Sensor Bridge (HSB) IP as the
network IP.

## Layout

| Path | Contents |
|---|---|
| `third_party/hsb/` | NVIDIA's HSB IP with its license |
| `hw/rtl/hsb_cfg/HOLOLINK_def.svh` | HSB build configuration for the VPK120 |
| `hw/rtl/top/backline_hsb_top.sv` | board top that instantiates HSB |

## Build and check

The bitstream needs Vivado 2024.2. The checks need Verilator 5.048 or newer, and tclsh.

| Command | What it does |
|---|---|
| `make amd AMD="<dir>/reference design VPK120_mrmac_rev_1.3.zip"` | Unpacks the parts of AMD's reference design the build reads into `build/amd`. `WITH_BSP=1` also unpacks the PetaLinux BSP. |
| `make hw` | Creates the Vivado project from scratch and builds it. Writes `backline_hsb.xsa`, the `.pdi` and `reports/` to `build/vivado/backline_hsb/`. |
| `make project` | Only creates the Vivado project, for example to open it in the GUI. |
| `make check` | Runs `make lint`, `make sim` and `make dryrun`. Needs no Vivado. |
| `make lint` | Verilator lint of `backline_hsb_top`. |
| `make sim` | Runs the testbenches `tb_hwhs_engine` and `tb_hwhs_hsb`. |
| `make dryrun` | Runs `create_project.tcl` in tclsh with Vivado's commands mocked. Needs `make amd` first. |
| `make clean` | Deletes `build/`. |

Options for `make project` and `make hw`:

| Option | Default | What it sets |
|---|---|---|
| `HIF_MHZ` | 250 | HSB's hif clock and the hwhs engine's clock, in MHz |
| `JOBS` | 8 | Vivado's parallel jobs (`make hw` only) |
| `VIVADO` | `vivado` | the Vivado executable |

## Licensing

- `hw/` is Apache-2.0; see `LICENSE`. `hw/rtl/hsb_cfg/HOLOLINK_def.svh` is adapted from NVIDIA's HSB
   configuration.
- `third_party/hsb/` is NVIDIA's HSB IP under Apache-2.0; see `third_party/hsb/LICENSE`.

NVIDIA and Holoscan are trademarks of NVIDIA Corporation.
