// MS-A1 Embedded Controller Fan Reader - C Implementation
// Provides direct EC port access for Minisforum MS-A1
//
// Build: gcc -o ms_a1_ec_read ms_a1_ec_read.c
// Usage: sudo ./ms_a1_ec_read [command]
//
// Based on EC interface research from AIDA64 forums:
// - CMDIO port: 0x6C
// - DATAIO port: 0x68
// - Fan read command: 0xD5

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/io.h>
#include <sys/types.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <time.h>

// MS-A1 EC Interface (from AIDA64 forum research)
#define MSA1_EC_CMD_PORT  0x6C
#define MSA1_EC_DATA_PORT 0x68

// Standard ACPI EC ports (fallback)
#define ACPI_EC_DATA_PORT 0x62
#define ACPI_EC_CMD_PORT  0x66

// EC Status bits
#define EC_STAT_OBF 0x01  // Output Buffer Full
#define EC_STAT_IBF 0x02  // Input Buffer Full
#define EC_STAT_CMD 0x08  // Command mode

// EC Commands
#define EC_CMD_READ_FAN 0xD5
#define EC_CMD_READ_TEMP 0xDD

// Common fan register pairs from various implementations
static const uint16_t fan_registers[][2] = {
    {0x10, 0x11},  // ChromeOS EC
    {0x2E, 0x2F},  // LattePanda Sigma
    {0x22, 0x23},  // IT5570
    {0x84, 0x85},  // ThinkPad
    {0x66, 0x67},  // EeePC
    {0x35, 0x36},  // AXB35
};

#define NUM_FAN_REGS (sizeof(fan_registers) / sizeof(fan_registers[0]))

static int use_direct_io = 0;
static int ec_sys_fd = -1;

int init_ec_access(void) {
    // Try ec_sys debugfs first
    ec_sys_fd = open("/sys/kernel/debug/ec/ec0/io", O_RDONLY);
    if (ec_sys_fd >= 0) {
        printf("[+] Using ec_sys debugfs\n");
        return 0;
    }
    
    // Fall back to direct port I/O
    if (iopl(3) < 0) {
        perror("iopl failed (need root privileges)");
        return -1;
    }
    
    use_direct_io = 1;
    printf("[+] Using direct port I/O\n");
    return 0;
}

uint8_t read_ec_register_sys(uint8_t offset) {
    uint8_t value;
    if (pread(ec_sys_fd, &value, 1, offset) != 1) {
        perror("pread failed");
        return 0xFF;
    }
    return value;
}

int wait_ec_status(uint8_t mask, uint8_t expected, int timeout_ms) {
    for (int i = 0; i < timeout_ms; i++) {
        uint8_t status = inb(MSA1_EC_CMD_PORT);
        if ((status & mask) == expected) {
            return 0;
        }
        usleep(1000);  // 1ms
    }
    return -1;
}

uint8_t ec_transaction(uint8_t cmd, uint8_t data) {
    // Wait for EC ready
    if (wait_ec_status(EC_STAT_IBF, 0, 1000) < 0) {
        fprintf(stderr, "Timeout: EC not ready\n");
        return 0xFF;
    }
    
    // Send command
    outb(cmd, MSA1_EC_CMD_PORT);
    
    // Wait for command accepted
    if (wait_ec_status(EC_STAT_IBF, 0, 1000) < 0) {
        fprintf(stderr, "Timeout: Command not accepted\n");
        return 0xFF;
    }
    
    // Send data
    outb(data, MSA1_EC_DATA_PORT);
    
    // Wait for response
    if (wait_ec_status(EC_STAT_OBF, EC_STAT_OBF, 1000) < 0) {
        fprintf(stderr, "Timeout: No response\n");
        return 0xFF;
    }
    
    return inb(MSA1_EC_DATA_PORT);
}

uint8_t read_ec_register(uint8_t offset) {
    if (use_direct_io) {
        // For direct I/O, use EC transaction protocol
        // This assumes the EC supports read commands
        return ec_transaction(EC_CMD_READ_FAN, offset);
    } else {
        return read_ec_register_sys(offset);
    }
}

void dump_ec_space(void) {
    printf("\n=== EC Register Space Dump ===\n\n");
    printf("     00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F");
    printf("\n     ");
    for (int i = 0; i < 48; i++) printf("-");
    printf("\n");
    
    for (int row = 0; row < 256; row += 16) {
        printf("%02X:  ", row);
        for (int col = 0; col < 16; col++) {
            uint8_t val = read_ec_register(row + col);
            printf("%02X ", val);
        }
        printf("\n");
    }
}

void scan_fans(void) {
    printf("\n=== Scanning Known Fan Register Locations ===\n\n");
    
    for (size_t i = 0; i < NUM_FAN_REGS; i++) {
        uint8_t lo = read_ec_register(fan_registers[i][0]);
        uint8_t hi = read_ec_register(fan_registers[i][1]);
        
        uint16_t rpm_be = (hi << 8) | lo;
        uint16_t rpm_le = (lo << 8) | hi;
        
        printf("Regs 0x%02X/0x%02X: Raw=0x%02X%02X | BE=%u | LE=%u\n",
               fan_registers[i][0], fan_registers[i][1],
               hi, lo, rpm_be, rpm_le);
    }
}

void scan_changing(int duration_sec) {
    printf("\n=== Scanning for Changing Values (%ds) ===\n", duration_sec);
    printf("Press Ctrl+C to stop early\n\n");
    
    uint8_t initial[256];
    uint8_t changed[256] = {0};
    
    // Read initial state
    for (int i = 0; i < 256; i++) {
        initial[i] = read_ec_register(i);
    }
    
    printf("Initial state captured. Monitor changes...\n\n");
    
    time_t start = time(NULL);
    while (time(NULL) - start < duration_sec) {
        for (int i = 0; i < 256; i++) {
            uint8_t current = read_ec_register(i);
            if (current != initial[i] && !changed[i]) {
                printf("[CHANGE] Reg 0x%02X: 0x%02X -> 0x%02X\n",
                       i, initial[i], current);
                changed[i] = 1;
            }
        }
        usleep(100000);  // 100ms
    }
    
    printf("\n=== Summary ===\n");
    int num_changed = 0;
    for (int i = 0; i < 256; i++) {
        if (changed[i]) num_changed++;
    }
    printf("Total registers changed: %d\n", num_changed);
}

void monitor_continuous(void) {
    printf("\n=== Continuous Monitoring (Ctrl+C to stop) ===\n\n");
    
    while (1) {
        time_t now = time(NULL);
        struct tm *tm_info = localtime(&now);
        char time_str[16];
        strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);
        
        printf("[%s] ", time_str);
        
        // Read first 3 known fan locations
        for (int i = 0; i < 3 && i < (int)NUM_FAN_REGS; i++) {
            uint8_t lo = read_ec_register(fan_registers[i][0]);
            uint8_t hi = read_ec_register(fan_registers[i][1]);
            uint16_t rpm = (hi << 8) | lo;
            
            if (rpm > 0 && rpm < 10000) {
                printf("Fan%d:%u ", i+1, rpm);
            }
        }
        printf("\n");
        
        sleep(2);
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("MS-A1 Embedded Controller Fan Reader\n\n");
        printf("Usage: %s <command>\n\n", argv[0]);
        printf("Commands:\n");
        printf("  dump     - Dump entire EC register space\n");
        printf("  fans     - Scan known fan register locations\n");
        printf("  scan     - Scan for changing values (10s)\n");
        printf("  monitor  - Continuous monitoring\n");
        printf("\n");
        return 1;
    }
    
    if (geteuid() != 0) {
        fprintf(stderr, "[!] Warning: Not running as root. EC access may fail.\n");
        fprintf(stderr, "    Try: sudo %s %s\n\n", argv[0], argv[1]);
    }
    
    if (init_ec_access() < 0) {
        fprintf(stderr, "Failed to initialize EC access\n");
        fprintf(stderr, "Try: sudo modprobe ec_sys write_support=1\n");
        return 1;
    }
    
    if (strcmp(argv[1], "dump") == 0) {
        dump_ec_space();
    } else if (strcmp(argv[1], "fans") == 0) {
        scan_fans();
    } else if (strcmp(argv[1], "scan") == 0) {
        scan_changing(10);
    } else if (strcmp(argv[1], "monitor") == 0) {
        monitor_continuous();
    } else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        return 1;
    }
    
    if (ec_sys_fd >= 0) {
        close(ec_sys_fd);
    }
    
    return 0;
}
