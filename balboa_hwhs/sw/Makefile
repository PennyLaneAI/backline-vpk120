# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Xanadu Quantum Technologies Inc.
#
# Build balboactl and mrmac-up outside PetaLinux: natively on the board, or cross, e.g.
#   make CC=aarch64-linux-gnu-gcc
# The image build (sw/petalinux/build.sh) uses the same sources through the hwhs-tools recipe.
CFLAGS   ?= -O2 -Wall -Wextra
CPPFLAGS += -Ilib/hwhs -I../common/include
OUT    ?= build

HDRS = lib/hwhs/hwhs.h ../common/include/hwhs_regs.h ../common/include/balboa_handshake.h

all: $(OUT)/balboactl $(OUT)/mrmac-up

$(OUT)/balboactl: apps/balboactl/balboactl.c lib/hwhs/hwhs.c $(HDRS)
	@mkdir -p $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ apps/balboactl/balboactl.c lib/hwhs/hwhs.c

$(OUT)/mrmac-up: apps/mrmac-up/mrmac-up.c lib/hwhs/hwhs.c $(HDRS)
	@mkdir -p $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ apps/mrmac-up/mrmac-up.c lib/hwhs/hwhs.c

clean:
	rm -rf $(OUT)
