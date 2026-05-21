KERNEL_DIR := kernel
NET_FILTER_DIR := net_filter
BLOCK_FILTER_DIR := block_filter
USER_DIR := user

.PHONY: all kernel net_filter block_filter user clean

all: kernel net_filter block_filter user

kernel:
	$(MAKE) -C $(KERNEL_DIR)

net_filter: kernel
	$(MAKE) -C $(NET_FILTER_DIR)

block_filter: kernel
	$(MAKE) -C $(BLOCK_FILTER_DIR)

user:
	$(MAKE) -C $(USER_DIR)

clean:
	$(MAKE) -C $(KERNEL_DIR) clean
	$(MAKE) -C $(NET_FILTER_DIR) clean
	$(MAKE) -C $(BLOCK_FILTER_DIR) clean
	$(MAKE) -C $(USER_DIR) clean
