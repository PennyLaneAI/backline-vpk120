obj-m += hw_hs.o

SRC = $(CURDIR)

ifeq ($(ARCH), arm64)
ccflags-y += -DSQ_FROM_PL_DDR
endif

all:
	make -C $(KERNEL_SRC) M=$(SRC) KBUILD_EXTRA_SYMBOLS=$(SRC)/../../xib-module/1.0/Module.symvers

modules_install:
	make -C $(KERNEL_SRC) M=$(SRC) modules_install

clean:
	make -C $(KERNEL_SRC) M=$(SRC) clean
