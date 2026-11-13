# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Xanadu Quantum Technologies Inc.
# Build on the coprocessor host (needs rdma-core / libibverbs-dev).
#   AMD:    make                (ROCm's hipcc)
#   NVIDIA: make GPU=nvidia HIP_INCLUDE="<hip>/include <hipother>/hipnv/include"
#           (HIP's CUDA backend: nvcc with HIP's headers)
GPU          ?= amd
HIPCC        ?= hipcc
# MI210; `rocminfo | grep gfx` names another card's
OFFLOAD_ARCH ?= gfx90a
NVCC         ?= nvcc
# GH200; `nvidia-smi --query-gpu=compute_cap --format=csv` names another card's
CUDA_ARCH    ?= sm_90
HIP_INCLUDE  ?=
CXXFLAGS     ?= -O2 -Wall -Wextra
CPPFLAGS     += -I../../common/include -I../common

balboa-gpu-echo: balboa_gpu_echo.hip ../common/rc_peer.h ../../common/include/balboa_handshake.h
ifeq ($(GPU),nvidia)
	$(NVCC) -x cu -arch=$(CUDA_ARCH) -std=c++20 -D__HIP_PLATFORM_NVIDIA__ $(addprefix -I,$(HIP_INCLUDE)) \
	    $(CPPFLAGS) -O2 -Xcompiler -Wall,-Wextra $(LDFLAGS) -o $@ balboa_gpu_echo.hip -libverbs -lcuda
else
	$(HIPCC) --offload-arch=$(OFFLOAD_ARCH) -std=c++20 $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) \
	    -o $@ balboa_gpu_echo.hip -libverbs
endif

clean:
	rm -f balboa-gpu-echo
