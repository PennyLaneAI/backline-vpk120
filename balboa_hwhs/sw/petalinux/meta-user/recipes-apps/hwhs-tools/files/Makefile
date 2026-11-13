# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Xanadu Quantum Technologies Inc.
CFLAGS   ?= -O2
CFLAGS   += -Wall -Wextra
CPPFLAGS += -I.

all: balboactl mrmac-up

balboactl: balboactl.c hwhs.c hwhs.h hwhs_regs.h balboa_handshake.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ balboactl.c hwhs.c

mrmac-up: mrmac-up.c hwhs.c hwhs.h hwhs_regs.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ mrmac-up.c hwhs.c

clean:
	rm -f balboactl mrmac-up
