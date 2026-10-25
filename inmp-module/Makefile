BR_OUT     ?= $(CURDIR)/../buildroot/output
SRCS       := $(wildcard $(CURDIR)/*.c $(CURDIR)/*.h) $(CURDIR)/Kbuild

# Host build (default): make
KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build

# Cross build for the Buildroot Raspberry Pi 4 (aarch64) kernel: make arm64
ARM_KERNEL_DIR ?= $(BR_OUT)/build/linux-custom
ARM_CROSS      ?= $(BR_OUT)/host/bin/aarch64-buildroot-linux-gnu-

all:
	$(MAKE) modules-build KDIR=$(KERNEL_DIR) BUILD_DIR=$(CURDIR)/build

arm64:
	$(MAKE) modules-build KDIR=$(ARM_KERNEL_DIR) BUILD_DIR=$(CURDIR)/build-arm64 \
		ARCH=arm64 CROSS_COMPILE=$(ARM_CROSS)

modules-build:
	mkdir -p $(BUILD_DIR)
	cp -u $(SRCS) $(BUILD_DIR)/
	$(MAKE) -C $(KDIR) M=$(BUILD_DIR) $(if $(ARCH),ARCH=$(ARCH)) \
		$(if $(CROSS_COMPILE),CROSS_COMPILE=$(CROSS_COMPILE)) modules

clean:
	rm -rf $(CURDIR)/build $(CURDIR)/build-arm64

.PHONY: all arm64 modules-build clean
