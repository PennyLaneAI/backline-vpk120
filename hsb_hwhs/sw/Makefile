# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Xanadu Quantum Technologies Inc.
#
# Build hsbctl and mrmac-up outside PetaLinux, natively or cross:
#   make CC=aarch64-linux-gnu-gcc
CFLAGS   ?= -O2 -Wall -Wextra
CPPFLAGS += -Ilib/hwhs -I../common/include
OUT    ?= build

HDRS = lib/hwhs/hwhs.h ../common/include/hwhs_regs.h ../common/include/hsb_regs.h ../common/include/hsb_handshake.h

all: $(OUT)/hsbctl $(OUT)/mrmac-up

$(OUT)/hsbctl: apps/hsbctl/hsbctl.c lib/hwhs/hwhs.c $(HDRS)
	@mkdir -p $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ apps/hsbctl/hsbctl.c lib/hwhs/hwhs.c

$(OUT)/mrmac-up: apps/mrmac-up/mrmac-up.c lib/hwhs/hwhs.c $(HDRS)
	@mkdir -p $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ apps/mrmac-up/mrmac-up.c lib/hwhs/hwhs.c

clean:
	rm -rf $(OUT)
