KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
TELEMETRY_SYMVERS := $(PWD)/../kernel/Module.symvers
BIN_DIR := ../bin

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(PWD) KBUILD_EXTRA_SYMBOLS=$(TELEMETRY_SYMVERS) modules
	mkdir -p $(BIN_DIR)
	cp block_filter.ko $(BIN_DIR)/

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f $(BIN_DIR)/block_filter.ko
