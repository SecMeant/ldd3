ifneq ($(KERNELRELEASE),)
	obj-m := scull.o
else
	KERNELDIR ?= /lib/modules/$(shell uname -r)/build
	PWD := $(shell pwd)
default:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
clean:
	rm Module.symvers modules.order scull.ko scull.mod scull.mod.c scull.mod.o scull.o
endif

