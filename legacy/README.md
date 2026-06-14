# Legacy proof-of-concept tools

These are the original userspace experiments, kept for historical reference.
**None of them work on this machine** because Secure Boot puts the kernel in
`integrity` lockdown, which blocks all userspace I/O-port access. They are
superseded by the signed kernel module in [`../src/`](../src/).

| File | Approach | Why it fails |
|---|---|---|
| `ms_a1_ec_read.c` | `iopl(3)` + `inb`/`outb` | `iopl()` is blocked under lockdown |
| `ms_a1_ec_read_sb.c` | `/dev/port` read/write | `/dev/port` is blocked under lockdown (the `_sb` name is misleading) |
| `ms_a1_ec_reader.py` | `ec_sys` debugfs + `/dev/port` | MS-A1 has no ACPI EC, so `ec_sys` never creates `ec0/io`; `/dev/port` is blocked |

See [`../docs/FINDINGS.md`](../docs/FINDINGS.md) for the full explanation of why
kernel-space access is the only viable path.

The prebuilt binaries that used to sit here are intentionally **not** tracked in
git (see `.gitignore`); only the source is kept.
