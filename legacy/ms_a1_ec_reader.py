#!/usr/bin/env python3
"""
MS-A1 Embedded Controller Fan Reader POC
Reads fan speed and temperature data from Minisforum MS-A1 EC

Based on research from:
- AIDA64 forum thread revealing EC interface (ports 0x6C/0x68)
- Linux EC access methods (ec_sys debugfs, direct port I/O)
- Community reverse-engineering efforts

USAGE:
    sudo python3 ms_a1_ec_reader.py [command]

COMMANDS:
    dump        - Dump entire EC register space (256 bytes)
    fans        - Read fan speeds
    temps       - Read temperature sensors
    scan        - Scan for changing values (useful for finding fan regs)
    monitor     - Continuous monitoring of fans and temps

REQUIREMENTS:
    - Root privileges (for port I/O or ec_sys access)
    - ec_sys kernel module loaded (modprobe ec_sys write_support=1)
    OR
    - Direct port I/O permissions
"""

import sys
import os
import time
import struct
import argparse
from pathlib import Path

# MS-A1 EC Interface Configuration (from AIDA64 forum research)
MSA1_EC_CMD_PORT = 0x6C   # CMDIO port
MSA1_EC_DATA_PORT = 0x68  # DATAIO port

# EC Commands discovered for MS-A1
EC_CMD_READ_TEMP = 0xDD   # Temperature read command
EC_CMD_READ_FAN = 0xD5    # Fan speed read command

# Standard ACPI EC ports (fallback)
ACPI_EC_DATA_PORT = 0x62
ACPI_EC_CMD_PORT = 0x66

# EC Status Register bits
EC_STAT_OBF = 0x01  # Output Buffer Full
EC_STAT_IBF = 0x02  # Input Buffer Full
EC_STAT_CMD = 0x08  # Command mode (vs data mode)

# Common fan register locations from various EC implementations
KNOWN_FAN_REGISTERS = [
    (0x10, 0x11),  # ChromeOS EC: EC_MEMMAP_FAN (0x10-0x17 for 4 fans)
    (0x2E, 0x2F),  # LattePanda Sigma
    (0x22, 0x23),  # IT5570
    (0x84, 0x85),  # ThinkPad standard
    (0x66, 0x67),  # EeePC
    (0x35, 0x36),  # AXB35 Fan 1
]

class ECReader:
    """Embedded Controller reader with multiple access methods"""
    
    def __init__(self):
        self.method = None
        self.ec_sys_path = Path("/sys/kernel/debug/ec/ec0/io")
        self._detect_method()
    
    def _detect_method(self):
        """Detect best available EC access method"""
        # Try ec_sys debugfs first (safest)
        if self.ec_sys_path.exists():
            self.method = 'ec_sys'
            print(f"[+] Using ec_sys debugfs: {self.ec_sys_path}")
            return
        
        # Try direct port I/O (requires root and iopl)
        try:
            import ctypes
            libc = ctypes.CDLL('libc.so.6')
            if libc.iopl(3) == 0:
                self.method = 'port_io'
                print("[+] Using direct port I/O")
                return
        except:
            pass
        
        raise PermissionError(
            "No EC access method available.\n"
            "Try: sudo modprobe ec_sys write_support=1\n"
            "Or run with root privileges for port I/O"
        )
    
    def _read_port(self, port):
        """Read byte from I/O port (x86 only)"""
        import ctypes
        libc = ctypes.CDLL('libc.so.6')
        value = ctypes.c_uint8()
        # Using inb instruction via inline asm would be better, 
        # but for POC we'll use /dev/port if available
        with open('/dev/port', 'rb') as f:
            f.seek(port)
            return ord(f.read(1))
    
    def _write_port(self, port, value):
        """Write byte to I/O port"""
        with open('/dev/port', 'wb') as f:
            f.seek(port)
            f.write(bytes([value]))
    
    def _wait_ec(self, port, mask, expected, timeout=1000):
        """Wait for EC status bit with timeout"""
        for _ in range(timeout):
            status = self._read_port(port)
            if (status & mask) == expected:
                return True
            time.sleep(0.001)
        return False
    
    def _ec_transaction(self, cmd, data=None):
        """Perform EC transaction using direct port I/O"""
        # Wait for EC ready (IBF clear)
        if not self._wait_ec(MSA1_EC_CMD_PORT, EC_STAT_IBF, 0):
            raise TimeoutError("EC not ready for command")
        
        # Send command
        self._write_port(MSA1_EC_CMD_PORT, cmd)
        
        # Wait for command accepted
        if not self._wait_ec(MSA1_EC_CMD_PORT, EC_STAT_IBF, 0):
            raise TimeoutError("EC command not accepted")
        
        if data is not None:
            # Write data byte
            self._write_port(MSA1_EC_DATA_PORT, data)
            if not self._wait_ec(MSA1_EC_CMD_PORT, EC_STAT_IBF, 0):
                raise TimeoutError("EC data not accepted")
        
        # Wait for response (OBF set)
        if not self._wait_ec(MSA1_EC_CMD_PORT, EC_STAT_OBF, EC_STAT_OBF):
            raise TimeoutError("EC no response")
        
        # Read response
        return self._read_port(MSA1_EC_DATA_PORT)
    
    def read_ec_sys(self, offset, length=1):
        """Read EC register via ec_sys debugfs"""
        try:
            with open(self.ec_sys_path, 'rb') as f:
                f.seek(offset)
                data = f.read(length)
                return data[0] if length == 1 else data
        except Exception as e:
            raise IOError(f"Failed to read EC register 0x{offset:02X}: {e}")
    
    def read_register(self, offset, length=1):
        """Read EC register using detected method"""
        if self.method == 'ec_sys':
            return self.read_ec_sys(offset, length)
        elif self.method == 'port_io':
            # For port I/O, we need to use EC read command
            # This is implementation-specific
            return self._ec_transaction(EC_CMD_READ_FAN, offset)
        else:
            raise RuntimeError("No EC access method available")
    
    def dump_ec_space(self):
        """Dump entire 256-byte EC register space"""
        print("\n=== EC Register Space Dump ===\n")
        print("     00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F")
        print("     " + "-" * 48)
        
        for row in range(0, 256, 16):
            row_data = []
            for col in range(16):
                try:
                    val = self.read_register(row + col)
                    row_data.append(f"{val:02X}")
                except:
                    row_data.append("??")
            print(f"{row:02X}:  {' '.join(row_data)}")
    
    def read_fan_speed(self, register_pair):
        """Read fan RPM from 16-bit register pair"""
        try:
            lo = self.read_register(register_pair[0])
            hi = self.read_register(register_pair[1])
            
            # Try big-endian first (most common)
            rpm_be = (hi << 8) | lo
            # Try little-endian
            rpm_le = (lo << 8) | hi
            
            return {
                'lo_reg': register_pair[0],
                'hi_reg': register_pair[1],
                'lo_val': lo,
                'hi_val': hi,
                'rpm_be': rpm_be,
                'rpm_le': rpm_le,
                'raw': f"{hi:02X}{lo:02X}"
            }
        except Exception as e:
            return {'error': str(e)}
    
    def scan_fans(self):
        """Scan known fan register locations"""
        print("\n=== Scanning Known Fan Register Locations ===\n")
        
        for reg_pair in KNOWN_FAN_REGISTERS:
            result = self.read_fan_speed(reg_pair)
            
            if 'error' in result:
                print(f"Registers 0x{reg_pair[0]:02X}/0x{reg_pair[1]:02X}: Error - {result['error']}")
            else:
                print(f"Registers 0x{result['lo_reg']:02X}/0x{result['hi_reg']:02X}: "
                      f"Raw=0x{result['raw']} | BE={result['rpm_be']} | LE={result['rpm_le']}")
    
    def scan_changing_values(self, duration=10, interval=1):
        """Scan EC space for values that change (likely sensors)"""
        print(f"\n=== Scanning for Changing Values ({duration}s) ===\n")
        print("Press Ctrl+C to stop early\n")
        
        # Read initial state
        initial = {}
        for offset in range(256):
            try:
                initial[offset] = self.read_register(offset)
            except:
                initial[offset] = None
        
        print("Initial state captured. Monitor changes...\n")
        
        changed = set()
        start_time = time.time()
        
        try:
            while time.time() - start_time < duration:
                for offset in range(256):
                    try:
                        current = self.read_register(offset)
                        if initial[offset] is not None and current != initial[offset]:
                            if offset not in changed:
                                print(f"[CHANGE] Reg 0x{offset:02X}: 0x{initial[offset]:02X} -> 0x{current:02X}")
                                changed.add(offset)
                    except:
                        pass
                time.sleep(interval)
                
        except KeyboardInterrupt:
            print("\n\nScan interrupted by user")
        
        print(f"\n=== Summary ===")
        print(f"Total registers changed: {len(changed)}")
        if changed:
            print(f"Changed registers: {[f'0x{r:02X}' for r in sorted(changed)]}")
            
            # Check if any match known fan register pairs
            for reg_pair in KNOWN_FAN_REGISTERS:
                if reg_pair[0] in changed or reg_pair[1] in changed:
                    print(f"\n[!] Potential fan registers detected: 0x{reg_pair[0]:02X}/0x{reg_pair[1]:02X}")
    
    def monitor_continuous(self):
        """Continuous monitoring of fans and temps"""
        print("\n=== Continuous Monitoring (Ctrl+C to stop) ===\n")
        
        try:
            while True:
                print(f"\n[{time.strftime('%H:%M:%S')}]")
                
                # Try to read from common fan locations
                for i, reg_pair in enumerate(KNOWN_FAN_REGISTERS[:3]):
                    result = self.read_fan_speed(reg_pair)
                    if 'error' not in result and result['rpm_be'] > 0:
                        print(f"  Fan{i+1} @ 0x{reg_pair[0]:02X}/0x{reg_pair[1]:02X}: "
                              f"{result['rpm_be']} RPM (raw: 0x{result['raw']})")
                
                time.sleep(2)
                
        except KeyboardInterrupt:
            print("\n\nMonitoring stopped")


def test_ec_access():
    """Test basic EC access"""
    print("Testing EC access...")
    
    try:
        ec = ECReader()
        
        # Try to read first few registers
        for offset in range(5):
            val = ec.read_register(offset)
            print(f"  EC[0x{offset:02X}] = 0x{val:02X}")
        
        print("\n[+] EC access working!")
        return ec
        
    except Exception as e:
        print(f"\n[-] EC access failed: {e}")
        sys.exit(1)


def main():
    parser = argparse.ArgumentParser(
        description='MS-A1 Embedded Controller Fan Reader POC',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
    sudo python3 ms_a1_ec_reader.py dump
    sudo python3 ms_a1_ec_reader.py fans
    sudo python3 ms_a1_ec_reader.py scan
    sudo python3 ms_a1_ec_reader.py monitor
        """
    )
    
    parser.add_argument(
        'command',
        choices=['dump', 'fans', 'temps', 'scan', 'monitor'],
        help='Command to execute'
    )
    
    args = parser.parse_args()
    
    # Check root privileges
    if os.geteuid() != 0:
        print("[!] Warning: Not running as root. EC access may fail.")
        print("    Try: sudo python3 ms_a1_ec_reader.py", args.command)
        print()
    
    # Initialize EC reader
    ec = test_ec_access()
    
    # Execute command
    if args.command == 'dump':
        ec.dump_ec_space()
    
    elif args.command == 'fans':
        ec.scan_fans()
    
    elif args.command == 'temps':
        print("\n=== Temperature Sensors ===\n")
        print("Note: Temp register locations vary by EC firmware")
        print("Common locations: 0x60-0x70 range\n")
        
        for offset in range(0x60, 0x78):
            try:
                val = ec.read_register(offset)
                print(f"  Reg 0x{offset:02X}: 0x{val:02X} ({val}°C?)")
            except Exception as e:
                print(f"  Reg 0x{offset:02X}: Error - {e}")
    
    elif args.command == 'scan':
        ec.scan_changing_values(duration=30)
    
    elif args.command == 'monitor':
        ec.monitor_continuous()


if __name__ == '__main__':
    main()
