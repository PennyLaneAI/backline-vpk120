# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Xanadu Quantum Technologies Inc.
#
#   make amd AMD="<dir>/reference design VPK120_mrmac_rev_1.3.zip" [WITH_BSP=1]
#                                   unpack AMD's reference design (and its BSP) into build/amd
#   make hw [HIF_MHZ=250] [JOBS=8]  fresh Vivado project, bitstream, XSA (Vivado 2024.2)
#   make project                    only create the Vivado project
#   make check                      lint + sim + dryrun (no Vivado needed)
SHELL   := /bin/bash
REPO    := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BUILD   := $(REPO)/build
HIF_MHZ ?= 250
JOBS    ?= 8
VIVADO  ?= vivado
PROJ    := $(BUILD)/vivado/backline_hsb

.PHONY: amd project hw lint sim dryrun check clean

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

lint:
	OUT=$(BUILD)/sim hw/sim/run.sh lint

sim:
	OUT=$(BUILD)/sim hw/sim/run.sh sim

dryrun:
	tclsh hw/sim/tcl_dryrun.tcl $(BUILD)/amd/hw

check: lint sim dryrun

clean:
	rm -rf $(BUILD)
