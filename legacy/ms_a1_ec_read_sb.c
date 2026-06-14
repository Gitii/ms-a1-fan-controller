// MS-A1 EC Reader - Secure Boot Compatible Version
// Uses /dev/port instead of iopl() for Secure Boot compatibility
//
// Build: gcc -o ms_a1_ec_read_sb ms_a1_ec_read_sb.c
// Usage: sudo ./ms_a1_ec_read_sb [dump|fans|scan|monitor]

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <time.h>

// MS-A1 EC Interface ports
#define MSA1_EC_CMD_PORT  0x6C
#define MSA1_EC_DATA_PORT 0x68

// Standard ACPI EC fallback ports
#define ACPI_EC_DATA_PORT 0x62
#define ACPI_EC_CMD_PORT  0x66

// EC Status bits
#define EC_STAT_OBF 0x01
#define EC_STAT_IBF 0x02

// EC Commands
#define EC_CMD_READ_FAN 0xD5

static int port_fd = -1;

// Known fan register locations
static const uint16_t fan_registers[][2] = {
    {0x10, 0x11}, {0x2E, 0x2F}, {0x22, 0x23},
    {0x84, 0x85}, {0x66, 0x67}, {0x35, 0x36},
};

#define NUM_FAN_REGS (sizeof(fan_registers) / sizeof(fan_registers[0]))

// Read byte from I/O port via /dev/port
uint8_t inb_port(unsigned short port) {
    uint8_t value;
    if (lseek(port_fd, port, SEEK_SET) < 0) {
        perror("lseek");
        return 0xFF;
    }
    if (read(port_fd, &value, 1) != 1) {
        perror("read port");
        return 0xFF;
    }
    return value;
}

// Write byte to I/O port via /dev/port
void outb_port(uint8_t value, unsigned short port) {
    if (lseek(port_fd, port, SEEK_SET) < 0) {
        perror("lseek");
        return;
    }
    if (write(port_fd, &value, 1) != 1) {
        perror("write port");
    }
}

int init_port_access(void) {
    port_fd = open("/dev/port", O_RDWR);
    if (port_fd < 0) {
        perror("Cannot open /dev/port (need root privileges)");
        return -1;
    }
    printf("[+] Using /dev/port for Secure Boot compatibility\n");
    return 0;
}

int wait_ec_status(uint8_t mask, uint8_t expected, int timeout_ms) {
    for (int i = 0; i < timeout_ms; i++) {
        uint8_t status = inb_port(MSA1_EC_CMD_PORT);
        if ((status & mask) == expected) {
            return 0;
        }
        usleep(1000);
    }
    return -1;
}

uint8_t ec_read_register(uint8_t reg) {
    // Wait for EC ready
    if (wait_ec_status(EC_STAT_IBF, 0, 1000) < 0) {
        return 0xFF;
    }
    
    // Send read command
    outb_port(EC_CMD_READ_FAN, MSA1_EC_CMD_PORT);
    
    // Wait for command accepted
    if (wait_ec_status(EC_STAT_IBF, 0, 1000) < 0) {
        return 0xFF;
    }
    
    // Send register address
    outb_port(reg, MSA1_EC_DATA_PORT);
    
    // Wait for response
    if (wait_ec_status(EC_STAT_OBF, EC_STAT_OBF, 1000) < 0) {
        return 0xFF;
    }
    
    return inb_port(MSA1_EC_DATA_PORT);
}

void dump_ec_space(void) {
    printf("\n=== EC Register Space Dump ===\n\n");
    printf("     00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F\n");
    printf("     ");
    for (int i = 0; i < 48; i++) printf("-");
    printf("\n");
    
    for (int row = 0; row < 256; row += 16) {
        printf("%02X:  ", row);
        for (int col = 0; col < 16; col++) {
            uint8_t val = ec_read_register(row + col);
            printf("%02X ", val);
            usleep(1000);  // Small delay between reads
        }
        printf("\n");
    }
}

void scan_fans(void) {
    printf("\n=== Scanning Fan Register Locations ===\n\n");
    
    for (size_t i = 0; i < NUM_FAN_REGS; i++) {
        uint8_t lo = ec_read_register(fan_registers[i][0]);
        uint8_t hi = ec_read_register(fan_registers[i][1]);
        
        uint16_t rpm_be = (hi << 8) | lo;
        uint16_t rpm_le = (lo << 8) | hi;
        
        printf("Regs 0x%02X/0x%02X: Raw=0x%02X%02X | BE=%u | LE=%u\n",
               fan_registers[i][0], fan_registers[i][1],
               hi, lo, rpm_be, rpm_le);
    }
}

void scan_changing(int duration_sec) {
    printf("\n=== Scanning for Changing Values (%ds) ===\n", duration_sec);
    printf("Press Ctrl+C to stop\n\n");
    
    uint8_t initial[256];
    uint8_t changed[256] = {0};
    
    printf("Reading initial state...\n");
    for (int i = 0; i < 256; i++) {
        initial[i] = ec_read_register(i);
    }
    
    printf("Monitoring... (generate load now!)\n\n");
    
    time_t start = time(NULL);
    while (time(NULL) - start < duration_sec) {
        for (int i = 0; i < 256; i++) {
            uint8_t current = ec_read_register(i);
            if (current != initial[i] && !changed[i]) {
                printf("[CHANGE] Reg 0x%02X: 0x%02X -> 0x%02X\n",
                       i, initial[i], current);
                changed[i] = 1;
            }
        }
        usleep(50000);  // 50ms
    }
    
    printf("\n=== Summary ===\n");
    int num_changed = 0;
    for (int i = 0; i < 256; i++) {
        if (changed[i]) num_changed++;
    }
    printf("Registers changed: %d\n", num_changed);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("MS-A1 EC Reader (Secure Boot Compatible)\n\n");
        printf("Usage: %s <command>\n\n", argv[0]);
        printf("Commands:\n");
        printf("  dump     - Dump EC register space\n");
        printf("  fans     - Scan fan registers\n");
        printf("  scan     - Find changing values\n");
        return 1;
    }
    
    if (geteuid() != 0) {
        fprintf(stderr, "[!] Must run as root (for /dev/port access)\n");
        return 1;
    }
    
    if (init_port_access() < 0) {
        return 1;
    }
    
    if (strcmp(argv[1], "dump") == 0) {
        dump_ec_space();
    } else if (strcmp(argv[1], "fans") == 0) {
        scan_fans();
    } else if (strcmp(argv[1], "scan") == 0) {
        scan_changing(30);
    } else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        return 1;
    }
    
    close(port_fd);
    return 0;
}
