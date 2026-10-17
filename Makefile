KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build

# match the compiler the running kernel was built with (CachyOS: clang + LTO)
ifneq ($(shell grep -s '^CONFIG_CC_IS_CLANG=y' $(KDIR)/.config),)
KBUILD_LLVM := LLVM=1
endif

all: module tools

module:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/src $(KBUILD_LLVM) modules

tools:
	$(MAKE) -C tools

module-clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/src $(KBUILD_LLVM) clean

clean: module-clean
	$(MAKE) -C tools clean

.PHONY: all module tools module-clean clean
