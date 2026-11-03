# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Xanadu Quantum Technologies Inc.
# Build on the coprocessor host (needs ROCm's hipcc and rdma-core / libibverbs-dev).
HIPCC        ?= hipcc
# MI210; `rocminfo | grep gfx` names another card's
OFFLOAD_ARCH ?= gfx90a
CXXFLAGS     ?= -O2 -Wall -Wextra
CPPFLAGS     += -I../../common/include

hsb-gpu-echo: hsb_gpu_echo.hip ../../common/include/hsb_handshake.h
	$(HIPCC) --offload-arch=$(OFFLOAD_ARCH) -std=c++20 $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) \
	    -o $@ hsb_gpu_echo.hip -libverbs

clean:
	rm -f hsb-gpu-echo
