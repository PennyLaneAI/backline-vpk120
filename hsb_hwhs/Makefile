# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Xanadu Quantum Technologies Inc.
#
#   make amd AMD="<dir>/reference design VPK120_mrmac_rev_1.3.zip" [WITH_BSP=1]
#                                   unpack AMD's reference design (and its BSP) into build/amd
#   make hw [HIF_MHZ=250] [JOBS=8]  fresh Vivado project, bitstream, XSA (Vivado 2024.2)
#   make project                    only create the Vivado project, e.g. to open it in the GUI
#   make petalinux [BSP=<bsp>]      PetaLinux 2024.2 image and jtag-boot.tcl (source settings.sh first)
#   make boot                       boot the board over JTAG
#   make apps [CC=...]              hsbctl and mrmac-up outside PetaLinux
#   make host                       hsb-echo, on the coprocessor host (needs rdma-core)
#   make host-gpu                   hsb-gpu-echo, on the coprocessor host (needs ROCm too)
#   make check                      lint + sim + dryrun + test: everything that needs no Vivado
SHELL   := /bin/bash
REPO    := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BUILD   := $(REPO)/build
HIF_MHZ ?= 250
JOBS    ?= 8
VIVADO  ?= vivado
BSP     ?= $(BUILD)/amd/sw/xilinx-vpk120-xsct-v2024.2-final.bsp
PROJ    := $(BUILD)/vivado/backline_hsb

.PHONY: amd project hw petalinux boot apps host host-gpu lint sim dryrun test check clean

amd:
	@test -n "$(AMD)" || { echo 'usage: make amd AMD="<dir>/reference design VPK120_mrmac_rev_1.3.zip" [WITH_BSP=1]'; exit 1; }
	scripts/unpack-amd.sh "$(AMD)" $(if $(filter 1,$(WITH_BSP)),--bsp) "$(BUILD)/amd"

project:
	rm -rf $(PROJ)
	mkdir -p $(BUILD)/vivado
	cd $(BUILD)/vivado && $(VIVADO) -mode batch -source $(REPO)/hw/scripts/create_project.tcl \
	    -tclargs -amd $(BUILD)/amd/hw -out $(BUILD)/vivado -hif_mhz $(HIF_MHZ)

hw: project
	cd $(BUILD)/vivado && $(VIVADO) -mode batch -source $(REPO)/hw/scripts/build.tcl \
	    -tclargs -xpr $(PROJ)/backline_hsb.xpr -jobs $(JOBS)

petalinux:
	sw/petalinux/build.sh $(PROJ)/backline_hsb.xsa $(BSP) $(BUILD)/amd/sw/versal-vpk120-revB.dtsi \
	    $(BUILD)/petalinux

boot:
	cd $(BUILD)/petalinux/backline-hsb && xsdb jtag-boot.tcl

apps:
	$(MAKE) -C sw OUT=$(BUILD)/apps

host:
	$(MAKE) -C host/hsb-echo

host-gpu:
	$(MAKE) -C host/hsb-gpu-echo

lint:
	OUT=$(BUILD)/sim hw/sim/run.sh lint

sim:
	OUT=$(BUILD)/sim hw/sim/run.sh sim

dryrun:
	tclsh hw/sim/tcl_dryrun.tcl $(BUILD)/amd/hw

test:
	@mkdir -p $(BUILD)/test
	$(CC) -Wall -Wextra -Icommon/include -Isw/lib/hwhs -o $(BUILD)/test/test_ecb_frame sw/tests/test_ecb_frame.c
	$(BUILD)/test/test_ecb_frame

check: lint sim dryrun test

clean:
	rm -rf $(BUILD)
