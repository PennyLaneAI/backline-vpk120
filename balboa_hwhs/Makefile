# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Xanadu Quantum Technologies Inc.
#
#   make amd AMD="<dir>/reference design VPK120_mrmac_rev_1.3.zip" [WITH_BSP=1]
#                                   unpack AMD's reference design (and its BSP) into build/amd
#   make hls [NET_MHZ=250]          BALBOA's HLS cores as Vivado IP (Vitis HLS 2024.2)
#   make hw [NET_MHZ=250] [JOBS=8]  fresh Vivado project, bitstream, XSA (Vivado 2024.2);
#                                   runs the HLS step first for any core not built yet
#   make project                    only create the Vivado project, e.g. to open it in the GUI
#   make check                      lint + sim + dryrun: everything that needs no Vivado
SHELL     := /bin/bash
REPO      := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BUILD     := $(REPO)/build
NET_MHZ   ?= 250
JOBS      ?= 8
VIVADO    ?= vivado
PROJ      := $(BUILD)/vivado/backline_roce
HLS_IP    := $(BUILD)/hls/iprepo
HLS_CORES := rocev2 ip_handler mac_ip_encode arp_server_subnet

.PHONY: amd hls project hw lint sim dryrun check clean

amd:
	@test -n "$(AMD)" || { echo 'usage: make amd AMD="<dir>/reference design VPK120_mrmac_rev_1.3.zip" [WITH_BSP=1]'; exit 1; }
	scripts/unpack-amd.sh "$(AMD)" $(if $(filter 1,$(WITH_BSP)),--bsp) "$(BUILD)/amd"

hls:
	hw/scripts/build_hls.sh -net_mhz $(NET_MHZ) -out $(BUILD)/hls

$(HLS_IP)/%/net_mhz:
	hw/scripts/build_hls.sh -net_mhz $(NET_MHZ) -out $(BUILD)/hls $*

project: $(foreach c,$(HLS_CORES),$(HLS_IP)/$(c)/net_mhz)
	rm -rf $(PROJ)
	mkdir -p $(BUILD)/vivado
	cd $(BUILD)/vivado && $(VIVADO) -mode batch -source $(REPO)/hw/scripts/create_project.tcl \
	    -tclargs -amd $(BUILD)/amd/hw -hls $(HLS_IP) -out $(BUILD)/vivado -net_mhz $(NET_MHZ)

hw: project
	cd $(BUILD)/vivado && $(VIVADO) -mode batch -source $(REPO)/hw/scripts/build.tcl \
	    -tclargs -xpr $(PROJ)/backline_roce.xpr -jobs $(JOBS)

lint:
	OUT=$(BUILD)/sim hw/sim/run.sh lint

sim:
	OUT=$(BUILD)/sim hw/sim/run.sh sim

dryrun:
	tclsh hw/sim/tcl_dryrun.tcl $(BUILD)/amd/hw

check: lint sim dryrun

clean:
	rm -rf $(BUILD)
