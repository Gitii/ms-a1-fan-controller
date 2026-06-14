// SPDX-License-Identifier: GPL-2.0
/*
 * msa1_ec.c -- Minisforum MS-A1 Embedded Controller hwmon driver
 *
 * The MS-A1 mini-PC does NOT expose its fan/temperature embedded controller
 * via ACPI (no ECDT, no PNP0C09 device).  The EC is reachable only through
 * direct I/O port access at non-standard ports 0x6C (command) / 0x68 (data),
 * using vendor-specific commands 0xDD (temperature) and 0xD5 (fan RPM).
 *
 * Register map (from public reverse engineering on AIDA64 forum
 * thread #16404, post by JaxJiang 2025-10-15):
 *
 *     transport
 *     ---------
 *     CMD  = 0x6C   (write command byte)
 *     DATA = 0x68   (write offset / read result)
 *     status @ 0x6C: bit0 OBF (data ready), bit1 IBF (EC busy)
 *
 *     temperatures via cmd 0xDD
 *     -------------------------
 *     0x20 -> CPU DTS temperature   (signed degC)
 *     0x21 -> motherboard temperature (signed degC)
 *
 *     fan RPM via cmd 0xD5 (2 bytes per fan)
 *     -------------------------------------
 *     0x14 / 0x15 -> fan3 low / high byte
 *     0x16 / 0x17 -> fan2 low / high byte
 *     0x18 / 0x19 -> fan1 low / high byte
 *
 * The module is hard-gated by DMI to Minisforum MS-A1 systems; loading on
 * any other hardware would poke unknown registers in the i8042 / SuperIO
 * address space and is refused unless `force=1` is passed.
 *
 * Author:   germinii
 * License:  GPL-2.0
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dmi.h>
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/types.h>

#if !defined(CONFIG_X86)
#error "msa1_ec requires x86 (uses port I/O at 0x6C / 0x68)"
#endif

/* ------------------------------------------------------------------ */
/*  EC transport constants                                            */
/* ------------------------------------------------------------------ */

#define MSA1_CMD_PORT          0x6C
#define MSA1_DATA_PORT         0x68

#define MSA1_STATUS_OBF        BIT(0)  /* data available */
#define MSA1_STATUS_IBF        BIT(1)  /* EC busy */

#define MSA1_CMD_READ_TEMP     0xDD
#define MSA1_CMD_READ_FAN      0xD5

#define MSA1_REG_TEMP_CPU      0x20
#define MSA1_REG_TEMP_BOARD    0x21

/* Fan low byte offsets (high byte is +1).  Order matches hwmon channel index. */
#define MSA1_FAN_COUNT         3
static const u8 msa1_fan_lo_offset[MSA1_FAN_COUNT] = {
	0x18,  /* fan1 */
	0x16,  /* fan2 */
	0x14,  /* fan3 */
};

/* Total poll budget for one IBF/OBF wait.  100 ms is far more than any real EC
 * needs (typical EC transactions resolve in < 1 ms), but generous timeouts
 * keep us robust against transient firmware activity. */
#define MSA1_POLL_TIMEOUT_US   100000
#define MSA1_POLL_INTERVAL_US  10

/* Short timeout used by the discovery-mode debugfs dumps.  Many (cmd, offset)
 * pairs are not supported by this EC -- e.g. cmd 0xD5 only responds for the
 * fan RPM offsets 0x14..0x19 and times out everywhere else.  With the normal
 * 100 ms budget a full 256-byte dump would block for ~77 seconds.  2 ms is
 * still well above the typical response time for valid pairs. */
#define MSA1_PROBE_TIMEOUT_US  2000

/* Plausibility bounds for the initial probe so we refuse to register a hwmon
 * device with nonsense readings (which would indicate the EC didn't actually
 * respond, e.g. on a non-MS-A1 board loaded with force=1). */
#define MSA1_PROBE_TEMP_MIN_MC  (-20 * 1000)
#define MSA1_PROBE_TEMP_MAX_MC  (125 * 1000)

/* ------------------------------------------------------------------ */
/*  Module parameters                                                 */
/* ------------------------------------------------------------------ */

static bool big_endian;
module_param(big_endian, bool, 0644);
MODULE_PARM_DESC(big_endian,
	"Fan RPM byte order: 0 = little-endian (default, LSB at lower offset), 1 = big-endian");

static bool force;
module_param(force, bool, 0444);
MODULE_PARM_DESC(force,
	"Skip DMI match and load on any system.  Dangerous: writes to 0x6C/0x68 may glitch the i8042 keyboard controller on non-MS-A1 hardware.");

static bool debug;
module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "Log every EC transaction (very verbose)");

static bool allow_scan;
module_param(allow_scan, bool, 0444);
MODULE_PARM_DESC(allow_scan,
	"Expose /sys/kernel/debug/msa1_ec/scan_cmds.  Probes unknown EC command bytes; some may have side effects.  Off by default; set at load time with allow_scan=1.");

/* ------------------------------------------------------------------ */
/*  Driver state                                                      */
/* ------------------------------------------------------------------ */

struct msa1_ec {
	struct device  *dev;
	struct device  *hwmon;
	struct mutex    io_lock;     /* serialize EC transactions */
	struct dentry  *debugfs;
};

/* Single global platform device -- one EC per machine. */
static struct platform_device *msa1_pdev;

/* ------------------------------------------------------------------ */
/*  Low-level EC I/O                                                  */
/* ------------------------------------------------------------------ */

static int msa1_wait_status(u8 mask, u8 expected, unsigned int timeout_us)
{
	unsigned int i, iters = timeout_us / MSA1_POLL_INTERVAL_US;

	for (i = 0; i < iters; i++) {
		u8 s = inb(MSA1_CMD_PORT);

		if ((s & mask) == expected)
			return 0;
		udelay(MSA1_POLL_INTERVAL_US);
	}
	return -ETIMEDOUT;
}

/* Read one byte from the EC.  Caller must hold ec->io_lock.
 * timeout_us controls how long we wait at each IBF/OBF poll. */
static int __msa1_ec_read(struct msa1_ec *ec, u8 cmd, u8 offset, u8 *val,
			  unsigned int timeout_us)
{
	int ret;

	lockdep_assert_held(&ec->io_lock);

	/* Drain any stale data byte left in the EC's output buffer by a
	 * previous timed-out transaction; otherwise our first OBF wait would
	 * succeed immediately and we'd read garbage from a prior request. */
	if (inb(MSA1_CMD_PORT) & MSA1_STATUS_OBF)
		(void)inb(MSA1_DATA_PORT);

	ret = msa1_wait_status(MSA1_STATUS_IBF, 0, timeout_us);
	if (ret) {
		dev_dbg(ec->dev, "EC busy before cmd (cmd=%02x off=%02x)\n",
			cmd, offset);
		return ret;
	}

	outb(cmd, MSA1_CMD_PORT);

	ret = msa1_wait_status(MSA1_STATUS_IBF, 0, timeout_us);
	if (ret) {
		dev_dbg(ec->dev, "EC busy after cmd (cmd=%02x off=%02x)\n",
			cmd, offset);
		return ret;
	}

	outb(offset, MSA1_DATA_PORT);

	ret = msa1_wait_status(MSA1_STATUS_OBF, MSA1_STATUS_OBF, timeout_us);
	if (ret) {
		dev_dbg(ec->dev, "EC timeout waiting for data (cmd=%02x off=%02x)\n",
			cmd, offset);
		return ret;
	}

	*val = inb(MSA1_DATA_PORT);

	if (debug)
		dev_info(ec->dev, "EC read cmd=%02x off=%02x -> %02x\n",
			 cmd, offset, *val);
	return 0;
}

static int msa1_ec_read_one(struct msa1_ec *ec, u8 cmd, u8 offset, u8 *val)
{
	int ret;

	mutex_lock(&ec->io_lock);
	ret = __msa1_ec_read(ec, cmd, offset, val, MSA1_POLL_TIMEOUT_US);
	mutex_unlock(&ec->io_lock);
	return ret;
}

/* Same as msa1_ec_read_one but with a short timeout, for discovery dumps
 * where most (cmd, offset) pairs are expected to fail. */
static int msa1_ec_probe_one(struct msa1_ec *ec, u8 cmd, u8 offset, u8 *val)
{
	int ret;

	mutex_lock(&ec->io_lock);
	ret = __msa1_ec_read(ec, cmd, offset, val, MSA1_PROBE_TIMEOUT_US);
	mutex_unlock(&ec->io_lock);
	return ret;
}

/* Read a 2-byte fan RPM.  We hold the lock across BOTH bytes to make sure
 * the EC doesn't change the value between low- and high-byte reads, and
 * snapshot the user-settable byte-order knob once so a mid-read toggle
 * can't produce a half-flipped result. */
static int msa1_read_fan_rpm(struct msa1_ec *ec, int idx, u16 *rpm)
{
	bool be = READ_ONCE(big_endian);
	u8 lo_off, b0, b1;
	int ret;

	if (idx < 0 || idx >= MSA1_FAN_COUNT)
		return -EINVAL;

	lo_off = msa1_fan_lo_offset[idx];

	mutex_lock(&ec->io_lock);
	ret = __msa1_ec_read(ec, MSA1_CMD_READ_FAN, lo_off, &b0,
			     MSA1_POLL_TIMEOUT_US);
	if (!ret)
		ret = __msa1_ec_read(ec, MSA1_CMD_READ_FAN, lo_off + 1, &b1,
				     MSA1_POLL_TIMEOUT_US);
	mutex_unlock(&ec->io_lock);

	if (ret)
		return ret;

	/* b0 = byte at lower offset, b1 = byte at higher offset. */
	*rpm = be ? ((u16)b0 << 8 | b1) : ((u16)b1 << 8 | b0);
	return 0;
}

static int msa1_read_temp_mc(struct msa1_ec *ec, u8 offset, long *millideg)
{
	u8 val;
	int ret;

	ret = msa1_ec_read_one(ec, MSA1_CMD_READ_TEMP, offset, &val);
	if (ret)
		return ret;

	/* EC reports temperature as signed degrees Celsius. */
	*millideg = (long)(s8)val * 1000;
	return 0;
}

/* ------------------------------------------------------------------ */
/*  hwmon ABI                                                         */
/* ------------------------------------------------------------------ */

static const char * const msa1_temp_labels[] = { "CPU", "Board" };
static const char * const msa1_fan_labels[]  = { "Fan 1", "Fan 2", "Fan 3" };

static umode_t msa1_is_visible(const void *drvdata,
			       enum hwmon_sensor_types type,
			       u32 attr, int channel)
{
	switch (type) {
	case hwmon_temp:
		if (channel >= (int)ARRAY_SIZE(msa1_temp_labels))
			return 0;
		if (attr == hwmon_temp_input || attr == hwmon_temp_label)
			return 0444;
		break;
	case hwmon_fan:
		if (channel >= MSA1_FAN_COUNT)
			return 0;
		if (attr == hwmon_fan_input || attr == hwmon_fan_label)
			return 0444;
		break;
	default:
		break;
	}
	return 0;
}

static int msa1_hwmon_read(struct device *dev,
			   enum hwmon_sensor_types type,
			   u32 attr, int channel, long *val)
{
	struct msa1_ec *ec = dev_get_drvdata(dev);
	u16 rpm;
	int ret;

	switch (type) {
	case hwmon_temp:
		if (attr != hwmon_temp_input)
			return -EOPNOTSUPP;
		if (channel == 0)
			return msa1_read_temp_mc(ec, MSA1_REG_TEMP_CPU, val);
		if (channel == 1)
			return msa1_read_temp_mc(ec, MSA1_REG_TEMP_BOARD, val);
		return -EINVAL;

	case hwmon_fan:
		if (attr != hwmon_fan_input)
			return -EOPNOTSUPP;
		ret = msa1_read_fan_rpm(ec, channel, &rpm);
		if (ret)
			return ret;
		*val = rpm;
		return 0;

	default:
		return -EOPNOTSUPP;
	}
}

static int msa1_hwmon_read_string(struct device *dev,
				  enum hwmon_sensor_types type,
				  u32 attr, int channel, const char **str)
{
	switch (type) {
	case hwmon_temp:
		if (attr == hwmon_temp_label &&
		    channel < (int)ARRAY_SIZE(msa1_temp_labels)) {
			*str = msa1_temp_labels[channel];
			return 0;
		}
		break;
	case hwmon_fan:
		if (attr == hwmon_fan_label && channel < MSA1_FAN_COUNT) {
			*str = msa1_fan_labels[channel];
			return 0;
		}
		break;
	default:
		break;
	}
	return -EOPNOTSUPP;
}

static const struct hwmon_ops msa1_hwmon_ops = {
	.is_visible  = msa1_is_visible,
	.read        = msa1_hwmon_read,
	.read_string = msa1_hwmon_read_string,
};

static const struct hwmon_channel_info * const msa1_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp,
		HWMON_T_INPUT | HWMON_T_LABEL,
		HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(fan,
		HWMON_F_INPUT | HWMON_F_LABEL,
		HWMON_F_INPUT | HWMON_F_LABEL,
		HWMON_F_INPUT | HWMON_F_LABEL),
	NULL,
};

static const struct hwmon_chip_info msa1_chip_info = {
	.ops  = &msa1_hwmon_ops,
	.info = msa1_hwmon_info,
};

/* ------------------------------------------------------------------ */
/*  debugfs: raw EC register dumps (discovery aid)                    */
/* ------------------------------------------------------------------ */

static int msa1_dump_show(struct seq_file *s, u8 cmd)
{
	struct msa1_ec *ec = s->private;
	u8 row[16];
	bool valid[16];
	int i, r, ret;
	int responded = 0;

	seq_printf(s,
		"EC dump via cmd 0x%02X  (-- = no response within %u us)\n",
		cmd, MSA1_PROBE_TIMEOUT_US);
	seq_puts(s, "      00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F\n");
	seq_puts(s, "      -----------------------------------------------\n");

	for (r = 0; r < 0x100; r += 0x10) {
		if (signal_pending(current))
			return -EINTR;

		for (i = 0; i < 16; i++) {
			ret = msa1_ec_probe_one(ec, cmd, r + i, &row[i]);
			valid[i] = (ret == 0);
			if (valid[i])
				responded++;
		}

		seq_printf(s, "%02X:   ", r);
		for (i = 0; i < 16; i++) {
			if (valid[i])
				seq_printf(s, "%02X ", row[i]);
			else
				seq_puts(s, "-- ");
		}
		seq_putc(s, '\n');
		cond_resched();
	}

	seq_printf(s, "\n%d / 256 offsets responded to cmd 0x%02X\n",
		   responded, cmd);
	return 0;
}

static int msa1_dump_d5_show(struct seq_file *s, void *unused)
{
	return msa1_dump_show(s, MSA1_CMD_READ_FAN);
}
DEFINE_SHOW_ATTRIBUTE(msa1_dump_d5);

static int msa1_dump_dd_show(struct seq_file *s, void *unused)
{
	return msa1_dump_show(s, MSA1_CMD_READ_TEMP);
}
DEFINE_SHOW_ATTRIBUTE(msa1_dump_dd);

/* ------------------------------------------------------------------ */
/*  debugfs: command-byte sweep                                       */
/* ------------------------------------------------------------------ */

/* Candidate command bytes ranked by prior probability the MS-A1 EC
 * understands them.  Sources:
 *   - ACPI EC spec (0x80..0x84)
 *   - ITE IT85xx / IT8528 / IT8987 reverse engineering (0x85..0x9F, 0x88)
 *   - The known MS-A1 read commands (0xD5, 0xDD) plus adjacent bytes that
 *     might be sibling read/write opcodes on the same firmware. */
static const u8 msa1_scan_cmds_list[] = {
	0x80, 0x81, 0x82, 0x83, 0x84,
	0x85, 0x86, 0x87, 0x88, 0x8F,
	0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
	0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F,
	0xD0, 0xD1, 0xD2, 0xD3, 0xD4,
	0xD5,
	0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xDB, 0xDC,
	0xDD,
	0xDE, 0xDF,
	0x10, 0x11,
};

/* Offsets probed per command.  Mix of known-responsive (validates the test
 * harness and identifies sibling read commands by matching values) and a
 * few exploratory points. */
static const u8 msa1_scan_offsets_list[] = {
	0x00, 0x14, 0x18, 0x20, 0x21, 0x30, 0x40,
};

/* Some ACPI EC commands have known write or state-changing semantics; if
 * we sent them with an arbitrary offset byte as the "value", we'd actually
 * be modifying EC state.  Hard-skip those bytes in the scan. */
static bool msa1_scan_cmd_unsafe(u8 cmd)
{
	switch (cmd) {
	case 0x81: /* ACPI WR_EC -- writes value byte */
	case 0x82: /* ACPI Burst Enable -- changes EC mode */
	case 0x83: /* ACPI Burst Disable -- changes EC mode */
		return true;
	default:
		return false;
	}
}

static int msa1_scan_cmds_show(struct seq_file *s, void *unused)
{
	struct msa1_ec *ec = s->private;
	int ci, oi;

	seq_printf(s,
		"EC command-byte scan -- probe timeout %u us per (cmd, offset)\n",
		MSA1_PROBE_TIMEOUT_US);
	seq_puts(s,
		"Legend: hex = EC responded with that value; -- = no response; SKIP = unsafe cmd\n");
	seq_puts(s,
		"WARNING: probing unknown commands may have side effects on the EC.\n\n");

	seq_puts(s, "        ");
	for (oi = 0; oi < (int)ARRAY_SIZE(msa1_scan_offsets_list); oi++)
		seq_printf(s, " 0x%02X", msa1_scan_offsets_list[oi]);
	seq_putc(s, '\n');
	seq_puts(s, "        ");
	for (oi = 0; oi < (int)ARRAY_SIZE(msa1_scan_offsets_list); oi++)
		seq_puts(s, " ----");
	seq_putc(s, '\n');

	for (ci = 0; ci < (int)ARRAY_SIZE(msa1_scan_cmds_list); ci++) {
		u8 cmd = msa1_scan_cmds_list[ci];

		if (signal_pending(current))
			return -EINTR;

		seq_printf(s, "  0x%02X:", cmd);

		if (msa1_scan_cmd_unsafe(cmd)) {
			for (oi = 0; oi < (int)ARRAY_SIZE(msa1_scan_offsets_list); oi++)
				seq_puts(s, " SKIP");
			seq_putc(s, '\n');
			continue;
		}

		for (oi = 0; oi < (int)ARRAY_SIZE(msa1_scan_offsets_list); oi++) {
			u8 val;
			int ret = msa1_ec_probe_one(ec, cmd,
				msa1_scan_offsets_list[oi], &val);
			if (ret == 0)
				seq_printf(s, "   %02X", val);
			else
				seq_puts(s, "   --");
		}
		seq_putc(s, '\n');
		cond_resched();
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(msa1_scan_cmds);

static void msa1_debugfs_setup(struct msa1_ec *ec)
{
	ec->debugfs = debugfs_create_dir(KBUILD_MODNAME, NULL);
	if (IS_ERR(ec->debugfs)) {
		ec->debugfs = NULL;
		return;
	}
	debugfs_create_file("dump_d5", 0400, ec->debugfs, ec, &msa1_dump_d5_fops);
	debugfs_create_file("dump_dd", 0400, ec->debugfs, ec, &msa1_dump_dd_fops);
	if (allow_scan) {
		debugfs_create_file("scan_cmds", 0400, ec->debugfs, ec,
				    &msa1_scan_cmds_fops);
		dev_info(ec->dev,
			"scan_cmds debugfs exposed (allow_scan=1) -- probes unknown EC commands\n");
	}
}

/* ------------------------------------------------------------------ */
/*  Platform driver                                                   */
/* ------------------------------------------------------------------ */

static int msa1_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct msa1_ec *ec;
	long t = 0;
	int ret;

	ec = devm_kzalloc(dev, sizeof(*ec), GFP_KERNEL);
	if (!ec)
		return -ENOMEM;

	ec->dev = dev;
	mutex_init(&ec->io_lock);
	dev_set_drvdata(dev, ec);

	/* Sanity probe: read CPU temperature.  If the EC doesn't respond
	 * (e.g. force=1 on the wrong hardware) bail out before exposing a
	 * broken hwmon node. */
	ret = msa1_read_temp_mc(ec, MSA1_REG_TEMP_CPU, &t);
	if (ret) {
		dev_err(dev, "EC probe failed (%d) -- refusing to register\n", ret);
		return ret;
	}
	if (t < MSA1_PROBE_TEMP_MIN_MC || t > MSA1_PROBE_TEMP_MAX_MC) {
		dev_err(dev, "implausible CPU temp %ld m°C -- refusing to register\n", t);
		return -EIO;
	}
	dev_info(dev, "EC probe OK, CPU = %ld.%03ld °C\n",
		 t / 1000, (t < 0 ? -t : t) % 1000);

	ec->hwmon = devm_hwmon_device_register_with_info(dev, "msa1_ec", ec,
							 &msa1_chip_info, NULL);
	if (IS_ERR(ec->hwmon)) {
		ret = PTR_ERR(ec->hwmon);
		dev_err(dev, "hwmon registration failed: %d\n", ret);
		return ret;
	}

	msa1_debugfs_setup(ec);

	dev_info(dev, "loaded (RPM byte order=%s)\n", big_endian ? "BE" : "LE");
	return 0;
}

static void msa1_remove(struct platform_device *pdev)
{
	struct msa1_ec *ec = dev_get_drvdata(&pdev->dev);

	debugfs_remove_recursive(ec->debugfs);
	/* hwmon device + ec struct are devm-managed */
}

static struct platform_driver msa1_driver = {
	.probe  = msa1_probe,
	.remove = msa1_remove,
	.driver = {
		.name = KBUILD_MODNAME,
	},
};

/* ------------------------------------------------------------------ */
/*  DMI gate + module init                                            */
/* ------------------------------------------------------------------ */

static const struct dmi_system_id msa1_dmi_table[] = {
	{
		.ident = "Minisforum MS-A1",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR,
				  "Micro Computer (HK) Tech Limited"),
			DMI_MATCH(DMI_PRODUCT_NAME, "MS-A1"),
		},
	},
	{ }
};
MODULE_DEVICE_TABLE(dmi, msa1_dmi_table);

static int __init msa1_init(void)
{
	int ret;

	if (!dmi_first_match(msa1_dmi_table)) {
		if (!force) {
			pr_info("not running on a Minisforum MS-A1; not loading\n");
			return -ENODEV;
		}
		pr_warn("force=1: loading on non-MS-A1 hardware -- THIS MAY GLITCH KEYBOARD/EC\n");
	}

	ret = platform_driver_register(&msa1_driver);
	if (ret)
		return ret;

	msa1_pdev = platform_device_register_simple(KBUILD_MODNAME, -1, NULL, 0);
	if (IS_ERR(msa1_pdev)) {
		platform_driver_unregister(&msa1_driver);
		return PTR_ERR(msa1_pdev);
	}
	return 0;
}

static void __exit msa1_exit(void)
{
	platform_device_unregister(msa1_pdev);
	platform_driver_unregister(&msa1_driver);
}

module_init(msa1_init);
module_exit(msa1_exit);

MODULE_AUTHOR("germinii");
MODULE_DESCRIPTION("Minisforum MS-A1 embedded controller hwmon driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1.0");
