# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Xanadu Quantum Technologies Inc.
# Build on the coprocessor host (needs rdma-core / libibverbs-dev).
CFLAGS   ?= -O2 -Wall -Wextra
CPPFLAGS += -I../../common/include

hsb-echo: hsb_echo.c ../../common/include/hsb_handshake.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ hsb_echo.c -libverbs

clean:
	rm -f hsb-echo
