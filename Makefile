# SPDX-License-Identifier: GPL-2.0
#
# Top-level convenience Makefile.
#   make                 -> build the kernel module against the running kernel
#   make sign            -> sign the module with the MOK key (manual path)
#   make install         -> install signed module into /lib/modules/.../extra
#   make load            -> insmod the freshly built module
#   make dkms-install    -> register + build + install via DKMS (persistent)
#   make dkms-uninstall  -> remove the DKMS package
#   make setup-mok       -> generate + enroll the MOK signing key (one-time)
#   make status          -> show secure boot / MOK / module state at a glance
#   make user            -> build the userspace hwmon reader CLI
#   make clean           -> kbuild clean

KVER     ?= $(shell uname -r)
KDIR     ?= /lib/modules/$(KVER)/build
SRC_DIR  := src
USER_DIR := userspace
MODULE   := $(SRC_DIR)/msa1_ec.ko
USER_BIN := ms_a1_ec_read
PKG_NAME := msa1-ec
PKG_VER  := 0.1.0

MOK_DIR  ?= /var/lib/shim-signed/mok
MOK_PRIV := $(MOK_DIR)/msa1.priv
MOK_DER  := $(MOK_DIR)/msa1.der

.PHONY: all build sign install load unload reload user \
        setup-mok dkms-install dkms-uninstall clean status

all: build

build:
	$(MAKE) -C $(SRC_DIR)

sign: build
	@if [ ! -r $(MOK_PRIV) ] || [ ! -r $(MOK_DER) ]; then \
		echo "ERROR: MOK key not found at $(MOK_PRIV) / $(MOK_DER)."; \
		echo "       Run: sudo make setup-mok    (one-time)"; \
		exit 1; \
	fi
	sudo kmodsign sha512 $(MOK_PRIV) $(MOK_DER) $(MODULE)
	@echo "Signed $(MODULE)"

install: sign
	sudo install -d /lib/modules/$(KVER)/extra/
	sudo install -m 0644 $(MODULE) /lib/modules/$(KVER)/extra/
	sudo depmod -a
	@echo "Installed to /lib/modules/$(KVER)/extra/msa1_ec.ko"

load:
	-sudo modprobe -r msa1_ec 2>/dev/null
	sudo modprobe msa1_ec
	@sleep 1
	@dmesg | tail -5 | grep -i msa1_ec || echo "(no msa1_ec dmesg lines yet)"

unload:
	sudo modprobe -r msa1_ec

reload: install load

user: $(USER_BIN)

$(USER_BIN): $(USER_DIR)/ms_a1_ec_read.c
	gcc -Wall -O2 -std=c11 -o $@ $<

setup-mok:
	sudo bash packaging/setup-mok.sh

dkms-install:
	sudo bash packaging/install.sh

dkms-uninstall:
	-sudo dkms remove -m $(PKG_NAME) -v $(PKG_VER) --all
	-sudo rm -rf /usr/src/$(PKG_NAME)-$(PKG_VER)
	-sudo rm -f /etc/dkms/framework.conf.d/msa1.conf

status:
	@echo "=== Kernel ==="
	@uname -r
	@echo
	@echo "=== Secure Boot ==="
	@mokutil --sb-state 2>/dev/null || true
	@echo
	@echo "=== MOK key enrolled? ==="
	@if mokutil --list-enrolled 2>/dev/null | grep -q "msa1-ec module signing key"; then \
		echo "  YES - 'msa1-ec module signing key' enrolled"; \
	else \
		echo "  NO  - run 'sudo make setup-mok' then reboot"; \
	fi
	@echo
	@echo "=== Module loaded? ==="
	@lsmod | grep -E '^msa1_ec' || echo "  not loaded"
	@echo
	@echo "=== hwmon device? ==="
	@found=0; for h in /sys/class/hwmon/hwmon*; do \
		if [ -e $$h/name ] && [ "$$(cat $$h/name)" = "msa1_ec" ]; then \
			echo "  $$h -> msa1_ec"; found=1; \
		fi; \
	done; [ $$found = 1 ] || echo "  not present"
	@echo
	@echo "=== DKMS status ==="
	@dkms status -m $(PKG_NAME) 2>/dev/null || echo "  (no DKMS package registered)"

DSDT_DIR ?= /tmp/msa1-dsdt

# Use devbox-provided iasl if present, otherwise fall back to system PATH.
IASL := $(shell test -x .devbox/nix/profile/default/bin/iasl && echo .devbox/nix/profile/default/bin/iasl || command -v iasl)

dsdt:
	@if [ -z "$(IASL)" ]; then \
		echo "iasl not found in devbox or PATH."; \
		echo "Run: devbox install   (acpica-tools is in devbox.json)"; \
		exit 1; \
	fi
	@echo "Using iasl: $(IASL)"
	@mkdir -p $(DSDT_DIR)
	@echo "=== Extracting ACPI tables ==="
	@sudo cp /sys/firmware/acpi/tables/DSDT $(DSDT_DIR)/DSDT.aml
	@for s in /sys/firmware/acpi/tables/SSDT*; do \
		n=$$(basename $$s); \
		sudo cp $$s $(DSDT_DIR)/$$n.aml; \
	done
	@sudo chown -R $(USER):$(USER) $(DSDT_DIR)
	@echo "=== Decompiling ==="
	@cd $(DSDT_DIR) && $(abspath $(IASL)) -d *.aml 2>&1 | grep -vE "^(Intel|Copyright|ASL\+|$$)" | tail -20 || true
	@echo
	@echo "=== Decompiled files ==="
	@ls $(DSDT_DIR)/*.dsl 2>/dev/null | xargs -I{} basename {} | tr '\n' ' '
	@echo
	@echo
	@echo "=== Total .dsl size ==="
	@du -ch $(DSDT_DIR)/*.dsl 2>/dev/null | tail -1
	@echo
	@echo "=== Fan / PWM / thermal symbols (Methods, Names, Fields) ==="
	@grep -hEo "(Method|Name|Field) \(([A-Z_0-9]*(FAN|PWM|TFAN|THML|THRM|FCTL|TACH|CTMP)[A-Z_0-9]*)" $(DSDT_DIR)/*.dsl 2>/dev/null | sort -u | head -60 || true
	@echo
	@echo "=== WMI methods (_WED / WMxx / WQxx / WSxx / WBxx) ==="
	@grep -hEo "Method \((WMNB|WMI[A-Z]*|_WED|W[QSB][A-Z0-9]{2})" $(DSDT_DIR)/*.dsl 2>/dev/null | sort -u | head -40 || true
	@echo
	@echo "=== AOD namespace methods (this is the AMD WMI surface!) ==="
	@grep -hE "(Scope \(.*AOD|Method \(.{0,50}AOD)" $(DSDT_DIR)/*.dsl 2>/dev/null | head -20 || true
	@echo
	@echo "=== _WDG buffer (WMI GUID descriptor) lines, anywhere ==="
	@grep -hB1 -A2 "_WDG" $(DSDT_DIR)/*.dsl 2>/dev/null | head -30 || true
	@echo
	@echo "=== Vendor-specific GUIDs ==="
	@grep -hEo '"[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}"' $(DSDT_DIR)/*.dsl 2>/dev/null | sort -u || true
	@echo
	@echo "=== SMI handler activity (port 0xB2 = APMC) ==="
	@grep -hE "(SMIC|SMI_|0xB2|0x00B2|APMC|SMIPort)" $(DSDT_DIR)/*.dsl 2>/dev/null | head -20 || true
	@echo
	@echo "=== References to our EC ports 0x68 / 0x6C ==="
	@grep -hE "0x0?0?68|0x0?0?6C" $(DSDT_DIR)/*.dsl 2>/dev/null | head -20 || true
	@echo
	@echo "=== Possible secondary EC ports ==="
	@grep -hE "(0x0?0?62|0x0?0?66|0x0?0?70|0x0?0?74|0x4E|0x4F|0x800|0x900|0xA20)" $(DSDT_DIR)/*.dsl 2>/dev/null | grep -iE "(IO|OperationRegion|Field)" | head -20 || true
	@echo
	@echo "=== OperationRegion (where BIOS defines accessible regions) ==="
	@grep -hE "OperationRegion \(" $(DSDT_DIR)/*.dsl 2>/dev/null | head -30 || true
	@echo
	@echo "Decompiled files live in $(DSDT_DIR)/*.dsl"
	@echo "AOD SSDT (most relevant): $(DSDT_DIR)/SSDT8.dsl  ($(shell stat -c '%s' /sys/firmware/acpi/tables/SSDT8 2>/dev/null) bytes raw)"

clean:
	$(MAKE) -C $(SRC_DIR) clean
	rm -f $(USER_BIN)

clean-dsdt:
	rm -rf $(DSDT_DIR)

.PHONY: dsdt clean-dsdt
