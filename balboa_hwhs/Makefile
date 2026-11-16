# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Xanadu Quantum Technologies Inc.
#
#   make amd AMD="<dir>/reference design VPK120_mrmac_rev_1.3.zip" [WITH_BSP=1]
#                                   unpack AMD's reference design (and its BSP) into build/amd
#   make hls [NET_MHZ=250]          BALBOA's HLS cores as Vivado IP (Vitis HLS 2024.2)
#   make hw [NET_MHZ=250] [JOBS=8]  fresh Vivado project, bitstream, XSA (Vivado 2024.2);
#                                   runs the HLS step first for any core not built yet
#   make project                    only create the Vivado project, e.g. to open it in the GUI
#   make petalinux [BSP=<bsp>]      PetaLinux 2024.2 image and jtag-boot.tcl (source settings.sh first)
#   make boot                       boot the board over JTAG
#   make apps [CC=...]              balboactl and mrmac-up outside PetaLinux
#   make host                       balboa-echo, on the coprocessor host (needs rdma-core)
#   make host-gpu                   balboa-gpu-echo, on the coprocessor host (needs ROCm too)
#   make host-standin               balboa-standin, the board's side of a session in software
#   make check [CSIM_CXX=g++-15]    lint + sim + dryrun + test + csim: everything that needs no Vivado
#                                   (csim needs GCC on macOS: brew install gcc)
SHELL     := /bin/bash
REPO      := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BUILD     := $(REPO)/build
NET_MHZ   ?= 250
JOBS      ?= 8
CSIM_CXX  ?= c++
VIVADO    ?= vivado
BSP       ?= $(BUILD)/amd/sw/xilinx-vpk120-xsct-v2024.2-final.bsp
PROJ      := $(BUILD)/vivado/backline_roce
HLS_IP    := $(BUILD)/hls/iprepo
HLS_CORES := rocev2 ip_handler mac_ip_encode arp_server_subnet

.PHONY: amd hls project hw petalinux boot apps host host-gpu host-standin lint sim csim dryrun test check clean

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

petalinux:
	sw/petalinux/build.sh $(PROJ)/backline_roce.xsa $(BSP) $(BUILD)/amd/sw/versal-vpk120-revB.dtsi \
	    $(BUILD)/petalinux

boot:
	cd $(BUILD)/petalinux/backline-roce && xsdb jtag-boot.tcl

apps:
	$(MAKE) -C sw OUT=$(BUILD)/apps

host:
	$(MAKE) -C host/balboa-echo

host-gpu:
	$(MAKE) -C host/balboa-gpu-echo

host-standin:
	$(MAKE) -C host/balboa-standin

lint:
	OUT=$(BUILD)/sim hw/sim/run.sh lint

sim:
	OUT=$(BUILD)/sim hw/sim/run.sh sim

csim:
	OUT=$(BUILD)/csim CXX="$(CSIM_CXX)" hw/hls/csim/run.sh

dryrun:
	tclsh hw/sim/tcl_dryrun.tcl $(BUILD)/amd/hw

test:
	@mkdir -p $(BUILD)/test
	$(CC) -Wall -Wextra -Icommon/include -Isw/lib/hwhs -o $(BUILD)/test/test_ctx_pack \
	    sw/tests/test_ctx_pack.c sw/lib/hwhs/hwhs.c
	$(BUILD)/test/test_ctx_pack

check: lint sim dryrun test csim

clean:
	rm -rf $(BUILD)
