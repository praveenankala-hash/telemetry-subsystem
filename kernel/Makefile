KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
BIN_DIR := ../bin

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	mkdir -p $(BIN_DIR)
	cp telemetry.ko $(BIN_DIR)/

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f $(BIN_DIR)/telemetry.ko
