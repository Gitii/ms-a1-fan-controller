# MS-A1 Embedded Controller: Reverse-Engineering Findings

A complete writeup of what we learned reverse-engineering the Minisforum MS-A1's
fan/temperature embedded controller (EC) on Linux, why a kernel module is required,
and why **fan control is not achievable** on this hardware from any OS.

- **Hardware:** Minisforum MS-A1 (AMD Ryzen, board `A5WSP`)
- **DMI:** `Micro Computer (HK) Tech Limited` / `MS-A1`
- **OS tested:** Zorin OS 18.1 (Ubuntu 24.04 base), kernel 6.17, Secure Boot ON
- **Result:** Read-only sensor driver (`msa1_ec`) — fans + temps via `hwmon`. No write path exists.

---

## TL;DR

1. The MS-A1 fan/temp EC is **not exposed through ACPI** (no ECDT, no `PNP0C09`).
   It is reachable only by direct x86 port I/O at the non-standard ports
   **`0x6C` (cmd) / `0x68` (data)**.
2. Under Secure Boot, the kernel runs in **`integrity` lockdown**, which blocks
   every userspace path to those ports (`iopl`, `/dev/port`, `/dev/mem`). The only
   way to talk to the EC is from **kernel space**, in a **MOK-signed module**.
3. The EC's command surface is **hard read-only**: exactly two commands respond
   (`0xD5` fans, `0xDD` temps), eight readable bytes total. A 308-probe command
   sweep found **no write commands, no general RAM gate, no sibling opcodes**.
4. The firmware exposes **no ACPI fan methods, no usable WMI fan interface, and no
   second EC**. Fan control happens entirely inside **SMM** (SMI port `0xB2`,
   8 MB control region at `0xFED80000`) using **proprietary, undocumented command
   codes** — inaccessible to any OS driver.
5. Conclusion: **Linux can read MS-A1 fans/temps but cannot control them.** This
   matches reality on Windows too — AIDA64's Feb 2026 beta added only *sensor*
   support for this EC.

---

## 1. Background: why the obvious approaches fail

### 1.1 No ACPI Embedded Controller

Standard PC fan/temp ECs are described in ACPI and Linux binds the `ec_sys`
debugfs driver to them at `/sys/kernel/debug/ec/ec0/io`. On the MS-A1:

```
/sys/firmware/acpi/tables/ECDT          → does not exist
/sys/bus/acpi/devices/PNP0C09:*         → does not exist
/sys/kernel/debug/ec/ec0/io             → never created (even after modprobe ec_sys)
dmesg | grep -i "embedded controller"   → empty
```

There is simply **no ACPI EC**. The kernel's exported `ec_read()` / `ec_write()`
helpers return `-ENODEV`. The fan controller lives on a separate, ACPI-invisible
interface.

### 1.2 The fan EC is at non-standard ports 0x6C / 0x68

Community reverse engineering on the [AIDA64 forum thread][aida] (notably the
post by *JaxJiang*, 2025-10-15) established the interface:

```
Transport (x86 port I/O)
  CMD  port = 0x6C        write command byte
  DATA port = 0x68        write offset / read result
  status @ 0x6C, bit 0  OBF: data available
  status @ 0x6C, bit 1  IBF: EC busy

Sequence (1-byte read)
  wait IBF=0
  outb(cmd,    0x6C)        cmd = 0xDD (temps) or 0xD5 (fans)
  wait IBF=0
  outb(offset, 0x68)
  wait OBF=1
  val = inb(0x68)

Temperatures via cmd 0xDD (signed °C)
  0x20 → CPU DTS temperature
  0x21 → motherboard temperature

Fan RPM via cmd 0xD5 (2 bytes per fan, little-endian)
  0x14 / 0x15 → fan3 (low / high)
  0x16 / 0x17 → fan2
  0x18 / 0x19 → fan1
```

### 1.3 Secure Boot lockdown blocks every userspace I/O path

The machine has Secure Boot enabled, which puts the kernel in `integrity`
lockdown (`/sys/kernel/security/lockdown` = `[integrity]`). Lockdown blocks,
regardless of any MOK enrollment:

| Userspace mechanism | Status under lockdown |
|---|---|
| `iopl(3)` / `ioperm()` | blocked |
| `/dev/port` read & write | blocked |
| `/dev/mem`, `/dev/kmem` | blocked |
| Loading unsigned kernel modules | blocked |

The original proof-of-concept tools (preserved in [`legacy/`](../legacy/)) all
fail here:

- `ms_a1_ec_read.c` — uses `iopl()` → **blocked**
- `ms_a1_ec_read_sb.c` — uses `/dev/port` (despite the `_sb` "secure boot" name) → **also blocked**
- the `ec_sys` fallback — never works because there is no ACPI EC

**Common misconception:** enrolling your own MOK key does *not* lift lockdown.
MOK only lets the kernel trust modules *you* sign. Lockdown stays active as long
as Secure Boot is on. The correct solution is therefore a **signed kernel module**
that does the port I/O from kernel space (where lockdown's userspace I/O
restrictions don't apply), exposed to userspace through the safe `hwmon` ABI.

---

## 2. The driver: `msa1_ec`

A small (~470 LoC) single-file hwmon driver. Design highlights:

- **Direct port I/O** at `0x6C`/`0x68` from kernel space, serialized with a mutex.
- **Stale-OBF drain** before every transaction (the EC can leave a byte in its
  output buffer after a timed-out probe; reading it first prevents returning
  garbage).
- **hwmon ABI**: `temp1` (CPU), `temp2` (Board), `fan1/2/3` with labels — so
  `lm-sensors`, `psensor`, GNOME, etc. work for free.
- **Hard DMI gate**: refuses to load on anything but an MS-A1, so it can't poke
  `0x6C`/`0x68` (which overlap the i8042 keyboard-controller window) on unrelated
  hardware.
- **debugfs discovery aids** (`dump_d5`, `dump_dd`, opt-in `scan_cmds`) used to
  perform the reverse engineering documented below.
- **MOK-signed** via DKMS so it survives kernel upgrades.

Verified working output:

```
$ sensors | sed -n '/msa1_ec/,/^$/p'
msa1_ec-isa-0000
Adapter: ISA adapter
Fan 1:       1580 RPM
Fan 2:       1533 RPM
Fan 3:       2351 RPM
CPU:          +48.0°C
Board:        +21.0°C
```

Decoded fan bytes match `sensors` to within sampling jitter, confirming the
little-endian byte order and the whole protocol model.

---

## 3. Hunting for a write path

Reading was solved. The real question for *fan control* was: **is there any
command that writes?** We attacked this from four directions.

### 3.1 Command-byte sweep (`scan_cmds`)

We added an opt-in debugfs sweep that probes ~45 candidate command bytes
(ACPI `0x80–0x84`, ITE-extended `0x85–0x9F`, the `0xD0–0xDF` neighborhood of the
known commands, MSI-vendor `0x10/0x11`) at seven representative offsets, with a
hard skip on the ACPI commands that have known write/state-change semantics
(`0x81` WR_EC, `0x82`/`0x83` burst enable/disable).

**Result — only two commands responded, at exactly the known offsets:**

```
         0x00 0x14 0x18 0x20 0x21 0x30 0x40
  0xD5:   --   44   D0   --   --   --   --     ← fan bytes only
  0xDD:   --   --   --   2E   1A   --   --     ← temp bytes only
  (every other command byte: -- everywhere)
```

No ACPI `0x80` RD_EC. No `0x84` QR_EC. No ITE `0x88`/`0x8F`/`0x90–0x9F`. No
sibling of `0xD5`/`0xDD`. **The EC genuinely implements only these two read
commands.** There is no write opcode to find on this port pair.

### 3.2 ACPI/DSDT analysis

We extracted and decompiled the full firmware (`DSDT` + 13 `SSDT`s, 3.8 MB of
ASL) with `iasl` and searched it. Findings:

| Search | Result |
|---|---|
| Fan/PWM/thermal method names (`FAN`,`PWM`,`THML`,`THRM`,`FCTL`,`TACH`) | **none** |
| ACPI fan-core methods (`_FST`,`_FIF`,`_FPS`,`_FSL`) | **none** |
| `0x6C`/`0x68` as `SystemIO` regions | **none** (only field offsets / method args) |
| Secondary EC ports (`0x62/0x66`, `0x4E/0x4F`, MMIO EC window) | **none** |

The BIOS never touches the EC through ACPI — it has no reason to, because it
drives the EC from SMM (below). That's *why* there's no ACPI EC device.

### 3.3 WMI analysis

The `\AOD_` ACPI device (`PNP0C14:00`, AMD OverDrive's WMI namespace) looked
promising — on many AMD systems this exposes power/thermal controls. But:

```
Name (_WDG, Buffer (0x28) { 0x6A,0x0F,0xBC,0xAB, ... })   ← exactly 1 GUID
                                                            = ABBC0F6A-8EA1-11D1-...
                                                            = standard WMI event consumer
```

`\AOD_` declares a **single, standard event-consumer GUID** — not a vendor
fan-control method. None of the 18 vendor `_DSM` GUIDs in the DSDT map to fan
logic (they're hardware-enumeration GUIDs: USB port typing, storage containers,
etc.). **AMD OverDrive's control surface is gutted on this consumer firmware.**

### 3.4 SMI/SMM: where control actually lives (and why we can't use it)

The DSDT *does* reveal an active SMM control surface:

```
Name (SMIO, 0xB2)                                            ← SMI command port (APMC)
OperationRegion (SMIC, SystemMemory, 0xFED80000, 0x00800000) ← 8 MB SMM region
OperationRegion (GSMM, SystemMemory, 0xFED80000, 0x1000)
APMC, 8,  SMIC, 8                                            ← SMI command field
```

This is almost certainly the path the firmware uses to drive the EC: software
writes a command code to port `0xB2`, triggering a System Management Interrupt;
the BIOS's SMM handler (running in ring -2) performs the actual EC write.

**But the SMI command codes are firmware-proprietary and undocumented.** Unlike
Dell's well-known SMM ABI (`drivers/hwmon/dell-smm-hwmon.c`), there is no public
map of MS-A1 SMI functions. Brute-forcing them is genuinely dangerous: a wrong
code can hang the machine or trigger destructive handlers. We chose **not** to go
down this path.

---

## 4. Why this is a firmware lock, not a missing driver

Putting it together:

```
   userspace
      │
   Linux kernel + msa1_ec        ← reads work (8 bytes via 0x6C/0x68)
      │ direct port I/O
   EC read-only surface          ← writes are silently refused
      ╎
      ╎ the ONLY write path is SMM:
      ╎   port 0xB2  +  8 MB control region @ 0xFED80000
      ╎   (proprietary SMI command codes — undocumented)
      │
   BIOS SMM handlers (ring -2, Minisforum proprietary)
      │
   BIOS fan curve  ← the only user-adjustable control, in BIOS setup
```

The fan controller is **owned by firmware**. No OS — Linux or Windows — gets a
documented write interface. The only user-facing fan control is the BIOS setup
fan-curve screen.

---

## 5. What *is* possible

Since direct fan control is off the table, the realistic options are:

### 5.1 Read-only monitoring (this project)
Ship `msa1_ec` as a signed read-only hwmon driver. `lm-sensors` and any hwmon
GUI get fan RPM + temps. **This is what this repository provides.**

### 5.2 Indirect thermal shaping (no extra reverse engineering)
The MS-A1 runs `amd-pstate-epp`. Capping CPU power/frequency or biasing the
energy-performance preference makes the CPU run cooler, so the BIOS auto fan
curve spins the fans down:

```bash
# bias toward power saving
echo power | sudo tee /sys/devices/system/cpu/cpufreq/policy*/energy_performance_preference
# or cap max frequency
echo 3000000 | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_max_freq
```

`thermald` can automate this. This is the kernel's documented answer for AMD
platforms without direct fan control
([`amd-pstate.rst`](https://docs.kernel.org/admin-guide/pm/amd-pstate.html)).

### 5.3 External microcontroller
For true fan control, intercept the fan PWM in hardware. The community
[kizzard/minisforum-ms-a1-fan-controller][kizzard] project drives the fan with an
external Trinket M0 + thermistor, bypassing the EC entirely.

---

## 6. Prior art & reference drivers

The kernel already has the patterns we'd want — they just need a *writable* EC,
which the MS-A1 doesn't provide:

| Reference | Relevance |
|---|---|
| [`drivers/platform/x86/msi-laptop.c`][msi] (`ec_read_only` quirk) | The exact "EC reads only, no writes" precedent. |
| [`drivers/hwmon/cros_ec_hwmon.c`][cros] | Canonical hwmon fan-control ABI (`pwm1`, `pwm1_enable`) — needs a writable EC. |
| [`drivers/hwmon/lattepanda-sigma-ec.c`][latte] | Mainline direct-port-I/O hwmon template (our structural model). |
| [`0xGiddi/qnap8528`][qnap] | Same `0x6C/0x68` transport on an ITE IT8528 — confirms the port convention. |
| [`passiveEndeavour/it5570-fan`][it5570] | Out-of-tree mini-PC EC hwmon driver with full fan control (writable EC). |
| [`drivers/hwmon/dell-smm-hwmon.c`][dellsmm] | The SMM-fan-control approach — needs documented SMI codes (MS-A1's are not). |

[aida]: https://forums.aida64.com/topic/16404-aida64-doesnt-detect-any-fans-of-minisforum-ms-a1-barebone/
[kizzard]: https://github.com/kizzard/minisforum-ms-a1-fan-controller
[msi]: https://github.com/torvalds/linux/blob/master/drivers/platform/x86/msi-laptop.c
[cros]: https://github.com/torvalds/linux/blob/master/drivers/hwmon/cros_ec_hwmon.c
[latte]: https://github.com/torvalds/linux/blob/master/drivers/hwmon/lattepanda-sigma-ec.c
[qnap]: https://github.com/0xGiddi/qnap8528
[it5570]: https://github.com/passiveEndeavour/it5570-fan
[dellsmm]: https://github.com/torvalds/linux/blob/master/drivers/hwmon/dell-smm-hwmon.c

---

## 7. Reproducing the analysis

```bash
# Read sensors (after installing the module — see the main README)
sensors | sed -n '/msa1_ec/,/^$/p'

# Discovery dumps
sudo cat /sys/kernel/debug/msa1_ec/dump_d5     # offset sweep, cmd 0xD5
sudo cat /sys/kernel/debug/msa1_ec/dump_dd     # offset sweep, cmd 0xDD

# Command-byte sweep (opt-in)
sudo modprobe -r msa1_ec
sudo modprobe msa1_ec allow_scan=1
sudo cat /sys/kernel/debug/msa1_ec/scan_cmds

# DSDT/SSDT decompile + grep for fan/WMI/SMI/EC symbols
make dsdt
```

---

*This document reflects the state of public knowledge and our own testing as of
mid-2026. If you discover a working MS-A1 fan-control path (e.g. decoded SMI
codes), please open an issue or PR — it would be the first.*
