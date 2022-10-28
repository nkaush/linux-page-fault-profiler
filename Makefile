CFLAGS_MODULE += -Wno-declaration-after-statement -Werror
APP_CFLAGS = -std=c11 -pipe -O2 -Werror

KERNEL_SRC := /lib/modules/$(shell uname -r)/build
SUBDIR := $(PWD)

CC ?= gcc

.PHONY: clean

all: clean modules monitor work

obj-m:= mp3.o

modules:
	$(MAKE) -C $(KERNEL_SRC) M=$(SUBDIR) modules

monitor: monitor.c
	$(CC) $(APP_CFLAGS) $< -o $@

work: work.c
	$(CC) $(APP_CFLAGS) $< -o $@

clean:
	rm -f monitor work *~ *.ko *.o *.mod.c Module.symvers modules.order


wipe: clean
	find . -name "*.cmd" -delete
	find . -name "*.mod" -delete
	find . -name "*.order" -delete

ul: unload load 

logs:
	sudo dmesg | grep "MP3"

slab_logs: 
	sudo cat /proc/slabinfo | head -n 10

load:
	sudo insmod mp3.ko

unload: 
	sudo rmmod mp3.ko

read:
	cat /proc/mp3/status