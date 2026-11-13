# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Xanadu Quantum Technologies Inc.
CFLAGS   ?= -O2
CFLAGS   += -Wall -Wextra
CPPFLAGS += -I.

all: hsbctl mrmac-up

hsbctl: hsbctl.c hwhs.c hwhs.h hwhs_regs.h hsb_regs.h hsb_handshake.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ hsbctl.c hwhs.c

mrmac-up: mrmac-up.c hwhs.c hwhs.h hwhs_regs.h hsb_regs.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ mrmac-up.c hwhs.c

clean:
	rm -f hsbctl mrmac-up
