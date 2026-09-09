obj-m += pl_allocator.o

pl_allocator-objs := pl_allocater.o

PWD := $(shell pwd)

#ccflags-y += -Wall -g -O2

default:
	make -C $(KERNEL_SRC) M=$(PWD)
modules_install:
	make -C $(KERNEL_SRC) M=$(PWD) modules_install

clean:
	make -C $(KERNEL_SRC) M=$(PWD) clean
