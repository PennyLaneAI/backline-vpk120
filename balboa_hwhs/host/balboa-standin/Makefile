# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Xanadu Quantum Technologies Inc.
# Build on any host with rdma-core / libibverbs-dev.
CFLAGS   ?= -O2 -Wall -Wextra
CPPFLAGS += -I../../common/include -I../common

balboa-standin: balboa_standin.c ../common/rc_peer.h ../../common/include/balboa_handshake.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ balboa_standin.c -libverbs

clean:
	rm -f balboa-standin
