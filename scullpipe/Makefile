ifneq ($(KERNELRELEASE),)
	obj-m := scullpipe.o
else
	KERNELDIR ?= /lib/modules/$(shell uname -r)/build
	PWD := $(shell pwd)
default:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
clean:
	rm Module.symvers modules.order scullpipe.ko scullpipe.mod scullpipe.mod.c scullpipe.mod.o scullpipe.o
endif
