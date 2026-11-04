obj-m += xilinx_ib.o

ifeq ($(ARCH), arm64)
xilinx_ib-objs := main.o rnic.o verbs.o mem.o axidma.o
else
xilinx_ib-objs := main.o rnic.o verbs.o mem.o axidma.o axi_timer.o
endif

SRC = $(CURDIR)
ccflags-y += -DNO_NVMF
ifeq ($(ARCH), arm64)
ccflags-y += -DSQ_FROM_PL_DDR
endif

all:
	make -C $(KERNEL_SRC) M=$(SRC) KBUILD_EXTRA_SYMBOLS="$(SRC)/../../xib-kmm/1.0/Module.symvers $(SRC)/../../pl-allocator/1.0/Module.symvers"

modules_install:
	make -C $(KERNEL_SRC) M=$(SRC) modules_install

clean:
	make -C $(KERNEL_SRC) M=$(SRC) clean
