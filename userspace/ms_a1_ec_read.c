// SPDX-License-Identifier: GPL-2.0
/*
 * ms_a1_ec_read.c -- userspace reader for the msa1_ec kernel module.
 *
 * Reads fan RPM and temperatures from /sys/class/hwmon/hwmonN/ where the
 * msa1_ec driver registered itself.  Works without root; no /dev/port, no
 * iopl(), no Secure Boot conflicts.
 */

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HWMON_BASE "/sys/class/hwmon"

static char hwmon_path[512];

static int read_file_line(const char *path, char *out, size_t outlen)
{
	FILE *f = fopen(path, "r");
	if (!f) return -1;
	if (!fgets(out, outlen, f)) { fclose(f); return -1; }
	out[strcspn(out, "\n")] = 0;
	fclose(f);
	return 0;
}

static int find_msa1_hwmon(void)
{
	DIR *d = opendir(HWMON_BASE);
	struct dirent *de;
	if (!d) { perror("opendir " HWMON_BASE); return -1; }

	while ((de = readdir(d))) {
		char name_path[600], name[64];
		if (strncmp(de->d_name, "hwmon", 5) != 0) continue;
		snprintf(name_path, sizeof(name_path), "%s/%s/name",
			 HWMON_BASE, de->d_name);
		if (read_file_line(name_path, name, sizeof(name)) == 0 &&
		    strcmp(name, "msa1_ec") == 0) {
			snprintf(hwmon_path, sizeof(hwmon_path), "%s/%s",
				 HWMON_BASE, de->d_name);
			closedir(d);
			return 0;
		}
	}
	closedir(d);
	return -1;
}

static long read_long_attr(const char *attr)
{
	char p[600], buf[32];
	snprintf(p, sizeof(p), "%s/%s", hwmon_path, attr);
	if (read_file_line(p, buf, sizeof(buf)) < 0) return LONG_MIN;
	return strtol(buf, NULL, 10);
}

static void read_string_attr(const char *attr, char *out, size_t outlen)
{
	char p[600];
	snprintf(p, sizeof(p), "%s/%s", hwmon_path, attr);
	if (read_file_line(p, out, outlen) < 0)
		snprintf(out, outlen, "(no label)");
}

int main(int argc, char **argv)
{
	int i;

	(void)argc; (void)argv;

	if (find_msa1_hwmon() < 0) {
		fprintf(stderr,
			"msa1_ec hwmon device not found.\n"
			"Is the kernel module loaded?\n"
			"  sudo modprobe msa1_ec\n");
		return 1;
	}
	printf("Found msa1_ec at %s\n\n", hwmon_path);

	printf("Temperatures:\n");
	for (i = 1; i <= 2; i++) {
		char label[64], attr[32];
		long mc;

		snprintf(attr, sizeof(attr), "temp%d_label", i);
		read_string_attr(attr, label, sizeof(label));
		snprintf(attr, sizeof(attr), "temp%d_input", i);
		mc = read_long_attr(attr);
		if (mc == LONG_MIN)
			printf("  %-10s = (unavailable)\n", label);
		else
			printf("  %-10s = %ld.%03ld degC\n", label,
			       mc / 1000, (mc < 0 ? -mc : mc) % 1000);
	}

	printf("\nFans:\n");
	for (i = 1; i <= 3; i++) {
		char label[64], attr[32];
		long rpm;

		snprintf(attr, sizeof(attr), "fan%d_label", i);
		read_string_attr(attr, label, sizeof(label));
		snprintf(attr, sizeof(attr), "fan%d_input", i);
		rpm = read_long_attr(attr);
		if (rpm == LONG_MIN)
			printf("  %-10s = (unavailable)\n", label);
		else
			printf("  %-10s = %ld RPM\n", label, rpm);
	}

	return 0;
}
