# SPDX-License-Identifier: GPL-2.0
#
# Out-of-tree build wrapper for the msa1_ec module.
# Drives kbuild against the running kernel by default; override KDIR for
# cross builds or testing against a different kernel tree.

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(CURDIR)

.PHONY: all clean modules modules_install

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
