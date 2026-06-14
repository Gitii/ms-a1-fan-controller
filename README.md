# Minisforum MS-A1 EC fan / temperature driver

Signed Linux kernel module (`msa1_ec`) exposing the proprietary Minisforum MS-A1
embedded controller via the standard `hwmon` sysfs ABI. Runs under UEFI Secure
Boot with kernel lockdown, no `iopl()`, no `/dev/port`, no userspace I/O hacks.

> ### Scope: read-only sensors
>
> This driver **reads** fan RPM and temperatures. It does **not** control the
> fans. We reverse engineered the EC exhaustively and confirmed the MS-A1's fan
> controller is **firmware-locked** — there is no fan-write path available to any
> OS (Linux *or* Windows). The full investigation, including the command-byte
> sweep, DSDT/WMI/SMI analysis, and why writes are impossible, is documented in
> **[docs/FINDINGS.md](docs/FINDINGS.md)**.
>
> If you need actual fan control, see the
> [workarounds](docs/FINDINGS.md#5-what-is-possible) (CPU power shaping via
> `amd-pstate`, or an external microcontroller).

## Why this exists

The MS-A1 BIOS does **not** expose its fan/temperature EC through ACPI - there
is no `ECDT` table, no `PNP0C09` device, and `/sys/kernel/debug/ec/ec0/io`
never appears. The EC is reachable only by direct I/O at the non-standard
ports `0x6C / 0x68` using vendor commands `0xDD` (temps) and `0xD5` (fans).

Under Secure Boot the kernel runs in `integrity` lockdown, which blocks every
userspace path to those ports (`iopl`, `/dev/port`, `/dev/mem`). The only way
to talk to the EC is from inside the kernel - hence this driver. Loading it
under Secure Boot requires signing with a MOK (Machine Owner Key) and
enrolling that key once.

## What you get

```
/sys/class/hwmon/hwmonN/   (where the msa1_ec device lands)
    name           = msa1_ec
    temp1_input    CPU DTS temperature   (millidegrees C)
    temp1_label    "CPU"
    temp2_input    Motherboard temperature
    temp2_label    "Board"
    fan1_input     Fan 1 RPM
    fan1_label     "Fan 1"
    fan2_input     Fan 2 RPM
    fan2_label     "Fan 2"
    fan3_input     Fan 3 RPM
    fan3_label     "Fan 3"

/sys/kernel/debug/msa1_ec/
    dump_d5        Full 256-byte EC dump via cmd 0xD5 (discovery aid)
    dump_dd        Full 256-byte EC dump via cmd 0xDD
```

Everything is read-only. Fan/PWM writes are **not possible** on this hardware —
the EC is firmware-locked and exposes no write path to any OS. See
[docs/FINDINGS.md](docs/FINDINGS.md) for the complete reverse-engineering writeup.

## Quick start

```bash
# 0. one-time prerequisites
sudo apt install -y dkms build-essential mokutil openssl \
                    linux-headers-$(uname -r) lm-sensors

# 1. one-time: generate + enroll the MOK signing key
sudo make setup-mok
sudo reboot                 # complete enrollment in the blue MOK Manager screen

# 2. after the reboot, verify enrollment
mokutil --list-enrolled | grep -A1 'msa1-ec'

# 3. build, sign, install via DKMS, and load
sudo make dkms-install

# 4. confirm
sensors | sed -n '/msa1_ec/,/^$/p'
make status
```

To uninstall:

```bash
sudo modprobe -r msa1_ec
sudo make dkms-uninstall
sudo mokutil --delete /var/lib/shim-signed/mok/msa1.der   # optional
```

## Build & sign matrix

| Command              | What it does                                                      |
|----------------------|-------------------------------------------------------------------|
| `make`               | kbuild against the running kernel (no install, no sign)           |
| `sudo make sign`     | sign the built `.ko` with `/var/lib/shim-signed/mok/msa1.{priv,der}` |
| `sudo make install`  | place signed `.ko` in `/lib/modules/$(uname -r)/extra/` + depmod  |
| `sudo make load`     | reload module, tail dmesg                                         |
| `sudo make reload`   | install + load                                                    |
| `sudo make setup-mok`| generate MOK keypair + request enrollment                          |
| `sudo make dkms-install`   | full DKMS workflow (kernel-update-resilient)                 |
| `sudo make dkms-uninstall` | remove DKMS package + framework config                       |
| `make status`        | one-page health summary                                           |
| `make user`          | build the userspace CLI reader (`ms_a1_ec_read`)                  |
| `make dsdt`          | extract + decompile DSDT/SSDTs and grep for fan/WMI/SMI symbols   |
| `make clean`         | kbuild clean                                                      |

## Layout

```
.
├── src/
│   ├── msa1_ec.c           kernel module (~470 LoC, single file)
│   ├── Kbuild              obj-m := msa1_ec.o
│   └── Makefile            kbuild wrapper for out-of-tree builds
├── userspace/
│   └── ms_a1_ec_read.c     dependency-free CLI reader (reads /sys/class/hwmon)
├── packaging/
│   ├── setup-mok.sh        one-time MOK keypair generation + enrollment
│   └── install.sh          one-shot DKMS install with sign config
├── docs/
│   └── FINDINGS.md         full reverse-engineering writeup (why writes are impossible)
├── dkms.conf               DKMS module manifest
├── Makefile                top-level convenience targets
├── devbox.json             devbox environment (gcc, make, openssl, lm_sensors)
└── LICENSE                 GPL-2.0
```

## Module parameters

```bash
sudo modprobe msa1_ec big_endian=1    # try if fan RPMs look wrong (~16k bogus)
sudo modprobe msa1_ec debug=1         # log every EC transaction (very noisy)
sudo modprobe msa1_ec force=1         # skip DMI check - DANGEROUS, see below
sudo modprobe msa1_ec allow_scan=1    # expose the scan_cmds discovery debugfs file
```

| Param         | Default | Effect                                                                                 |
|---------------|---------|----------------------------------------------------------------------------------------|
| `big_endian`  | 0 (LE)  | RPM byte order (LSB-at-low-offset vs MSB-at-low-offset). Toggle if RPM looks wrong.    |
| `debug`       | 0       | Verbose per-transaction `dev_info` logging.                                            |
| `force`       | 0       | Bypass DMI gate. **Will write to 0x6C/0x68 on any system** - keep off on non-MS-A1.   |
| `allow_scan`  | 0       | Expose `/sys/kernel/debug/msa1_ec/scan_cmds`. See *Discovery aids* below.              |

## Discovery aids

The module ships three debugfs files for reverse engineering the EC's command
set (mainly useful while building out fan-control / Phase 6):

| File         | Always present? | What it does                                                                 |
|--------------|-----------------|------------------------------------------------------------------------------|
| `dump_d5`    | yes             | 256-byte offset sweep using cmd `0xD5` (known fan-read).  Shows which offsets the fan command answers for. |
| `dump_dd`    | yes             | 256-byte offset sweep using cmd `0xDD` (known temp-read).                    |
| `scan_cmds`  | only when `allow_scan=1` | Sweeps ~45 candidate command bytes at a handful of known + exploratory offsets. Identifies which command bytes the EC ACKs.  |

```bash
# Always-on dumps:
sudo cat /sys/kernel/debug/msa1_ec/dump_d5
sudo cat /sys/kernel/debug/msa1_ec/dump_dd

# Command-byte sweep (requires opt-in, see safety notes below):
sudo modprobe -r msa1_ec
sudo modprobe msa1_ec allow_scan=1
sudo cat /sys/kernel/debug/msa1_ec/scan_cmds
```

### `scan_cmds` safety

The scanner only **reads** (cmd, offset, wait OBF, read data) — it never
issues writes of unknown semantics. It additionally **hard-skips** these
known-write/state-change ACPI EC opcodes:

| Skipped | Why |
|---------|-----|
| `0x81`  | ACPI WR_EC — writes value byte to EC RAM |
| `0x82`  | ACPI Burst Enable — changes EC mode |
| `0x83`  | ACPI Burst Disable — changes EC mode |

Even so, a "read" probe on an unknown firmware can have side effects if the
EC interprets our `offset` byte differently. The scanner is therefore
**off by default**.  Treat its output as a one-shot diagnostic, then unload
and reload without `allow_scan=1` for normal operation.

## Verification

After `modprobe msa1_ec`:

```bash
# 1. module loaded, signature accepted (kernel must NOT be tainted)
cat /proc/sys/kernel/tainted        # -> 0
lsmod | grep msa1_ec
dmesg | grep msa1_ec                # -> "EC probe OK, CPU = X.YYY °C", "loaded"

# 2. hwmon device present
ls /sys/class/hwmon/ | while read h; do
    [ "$(cat /sys/class/hwmon/$h/name 2>/dev/null)" = "msa1_ec" ] && echo $h
done

# 3. live readings
sensors | sed -n '/msa1_ec/,/^$/p'

# 4. RPM tracks load (run in a second terminal):
sudo apt install -y stress
stress --cpu 8 --timeout 60 &
watch -n1 'sensors | grep -A6 msa1_ec'

# 5. raw EC dump - useful for discovering the write registers
sudo cat /sys/kernel/debug/msa1_ec/dump_d5
sudo cat /sys/kernel/debug/msa1_ec/dump_dd
```

If `tainted` is non-zero or you see `Loading of unsigned module is rejected`
in dmesg, the signing chain broke - see [Troubleshooting](#troubleshooting).

## EC protocol reference

Reverse engineered from the [AIDA64 forum thread][aida-thread], post by
JaxJiang on 2025-10-15.

```
Transport (x86 port I/O)
────────────────────────
  CMD  port = 0x6C        (write command byte)
  DATA port = 0x68        (write offset / read result)

  status @ 0x6C, bit 0  OBF: data available
  status @ 0x6C, bit 1  IBF: EC busy

Sequence (1-byte read)
──────────────────────
  wait IBF=0
  outb(cmd, 0x6C)            cmd = 0xDD or 0xD5
  wait IBF=0
  outb(offset, 0x68)
  wait OBF=1
  val = inb(0x68)

Temperatures via cmd 0xDD (signed °C)
─────────────────────────────────────
  0x20 -> CPU DTS temperature
  0x21 -> motherboard temperature

Fan RPM via cmd 0xD5 (2 bytes per fan)
──────────────────────────────────────
  0x14 / 0x15 -> fan3 (low / high byte)
  0x16 / 0x17 -> fan2
  0x18 / 0x19 -> fan1
```

[aida-thread]: https://forums.aida64.com/topic/16404-aida64-doesnt-detect-any-fans-of-minisforum-ms-a1-barebone/

## Why userspace I/O doesn't work

The obvious approaches — `iopl(3)` + `inb`/`outb`, or `/dev/port` — are all
blocked by the kernel's **integrity lockdown** mode that ships with Secure Boot:

- `iopl(3)` / `ioperm()` -> blocked
- `/dev/port` read & write -> blocked
- `/dev/mem`, `/dev/kmem` -> blocked
- the `ec_sys` debugfs fallback would have worked, but MS-A1 doesn't expose an
  ACPI EC at all, so `/sys/kernel/debug/ec/ec0/io` is never created

Enrolling a MOK key does **not** lift lockdown - that's a common
misconception. MOK only lets the kernel trust modules you sign.
The only viable path is to do the port I/O from kernel space, which
is exactly what `msa1_ec` does.

## Troubleshooting

### `modprobe: ERROR: could not insert 'msa1_ec': Required key not available`
The kernel refused the module signature.
- Check the MOK is actually enrolled: `mokutil --list-enrolled | grep -A1 msa1-ec`
- Re-sign: `sudo make sign && sudo make install && sudo make load`

### `not running on a Minisforum MS-A1; not loading`
DMI didn't match. Verify with:
```bash
cat /sys/class/dmi/id/sys_vendor    # expect: Micro Computer (HK) Tech Limited
cat /sys/class/dmi/id/product_name  # expect: MS-A1
```
If you're sure this is an MS-A1 with a different DMI string, add a match
in `src/msa1_ec.c` -> `msa1_dmi_table` and rebuild.

### Fans report ~16000 RPM or nonsense values
The endianness might be wrong. Toggle the byte order:
```bash
sudo modprobe -r msa1_ec
sudo modprobe msa1_ec big_endian=1
```

### `implausible CPU temp -128 m°C — refusing to register`
The EC didn't actually respond. Likely causes:
- Loaded with `force=1` on non-MS-A1 hardware
- BIOS update changed the EC protocol (check BIOS revision)
- EC is in an odd state - try `sudo make unload && sudo make load`

### Module disappears after kernel update
Without DKMS, the `.ko` is tied to one kernel version.
Use the DKMS install path (`sudo make dkms-install`) and DKMS will rebuild
+ re-sign the module on every kernel upgrade.

## Why there is no fan control

We tried — exhaustively. A 308-probe command-byte sweep, full DSDT/SSDT
decompilation, WMI namespace inspection, and SMI/SMM analysis all confirm the
MS-A1's fan controller is **firmware-locked**: the EC exposes only two read
commands, the firmware has no ACPI fan methods, the AMD OverDrive WMI namespace
is a stub, and the only write path is through proprietary, undocumented SMI
handlers. No OS — Linux or Windows — can control these fans.

The complete investigation is in **[docs/FINDINGS.md](docs/FINDINGS.md)**.

For indirect control (CPU power shaping via `amd-pstate`) or hardware-level
control (external microcontroller), see the
[workarounds section](docs/FINDINGS.md#5-what-is-possible).

## Safety

- The DMI gate prevents accidental loading on other systems. Do not
  disable it (`force=1`) unless you're prepared to glitch that system's
  keyboard controller (the EC ports overlap the i8042 window).
- `scan_cmds` (opt-in via `allow_scan=1`) probes unknown EC command bytes.
  It only reads and hard-skips known-write ACPI commands, but treat it as a
  diagnostic and reload without `allow_scan` for normal use.

## License

GPL-2.0. See SPDX headers in each source file.
