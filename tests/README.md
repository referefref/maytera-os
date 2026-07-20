# MayteraOS Automated Testing Framework

This directory contains the automated testing framework for MayteraOS.

## Overview

The test framework provides:

- **VM Control**: Start, stop, reset VMs via Proxmox `qm` commands
- **Serial Communication**: Send commands and receive responses via serial socket
- **Test Suites**: Organized tests for different subsystems
- **Reporting**: JSON and HTML test reports

## Quick Start

```bash
# Run all tests
./run_all_tests.py --vm 1002

# Run specific test suite
./run_all_tests.py --vm 1002 --suite syscall

# Generate HTML report
./run_all_tests.py --vm 1002 --html-report

# List available tests
./run_all_tests.py --list

# Skip stress tests (faster)
./run_all_tests.py --vm 1002 --skip-stress
```

## Test Suites

### Basic Tests (`basic`)
- Serial echo functionality
- Help command
- Memory info
- Basic shell operations

### Syscall Tests (`syscall`)
- Process ID (getpid/getppid)
- Process scheduling (yield)
- Timer/sleep functionality
- File I/O (open, read, write)
- Memory allocation (brk, mmap)
- Time functions

### IPC Tests (`ipc`)
- Process creation and listing
- Process state tracking
- Priority management
- CPU time accounting
- Parent-child relationships
- Scheduler preemption
- Interrupt handling

### Capability Tests (`capability`)
- Kernel mode detection
- PCI bus access
- Memory protection
- ATA I/O ports
- Interrupt control
- ACPI access
- Network hardware access

### Stress Tests (`stress`)
- Rapid command execution
- Memory allocation stress
- Filesystem read stress
- Serial I/O stress
- Long-running stability
- Command flooding
- Heap fragmentation

## File Structure

```
tests/
├── test_runner.py       # Base test framework
├── run_all_tests.py     # Main entry point
├── syscall_tests.py     # Syscall test cases
├── ipc_tests.py         # IPC test cases
├── stress_tests.py      # Stress test cases
├── capability_tests.py  # Permission test cases
├── test_protocol.h      # In-OS test protocol
└── README.md            # This file
```

## Configuration

### VM Settings

The test framework expects:
- VM ID: 1002 (default, configurable via `--vm`)
- Serial socket: `/var/run/qemu-server/{vm_id}.serial0`
- Boot prompt: `maytera>`

### Timeouts

- Default serial timeout: 10 seconds
- Boot timeout: 90 seconds
- Individual test timeouts: 5-60 seconds (configurable per test)

## Output

### JSON Results

```json
[
  {
    "suite_name": "basic",
    "timestamp": "2026-01-29T10:30:00",
    "total_time_ms": 5432.1,
    "summary": {
      "total": 10,
      "passed": 9,
      "failed": 1,
      "skipped": 0,
      "errors": 0
    },
    "tests": [...]
  }
]
```

### HTML Report

A visual HTML report with:
- Overall pass rate
- Progress bar
- Suite-by-suite breakdown
- Individual test results
- Error messages

## Adding New Tests

### Python Test Function

```python
def test_my_feature(vm: VMController) -> Tuple[bool, str, str]:
    """Test description"""
    found, output = vm.send_command("my_command", expect="expected", timeout_sec=5)
    if found and "expected_output" in output:
        return True, output, ""
    return False, output, "Error message"
```

### Registering Tests

```python
framework.register_test(TestCase(
    name="my_feature_test",
    description="Test my feature",
    category="my_suite",
    test_func=test_my_feature,
    timeout_ms=5000
))
```

## In-OS Test Support

The `test_protocol.h` header provides assertion macros for in-kernel tests:

```c
#include "test_protocol.h"

test_result_t test_memory_alloc(void) {
    TEST_START("memory_alloc");

    void *ptr = kmalloc(1024);
    ASSERT_NOT_NULL(ptr);

    kfree(ptr);
    TEST_END();
}
```

## Troubleshooting

### VM Not Starting

```bash
# Check VM status
qm status 1002

# Start manually
qm start 1002

# Check logs
journalctl -u pveproxy
```

### Serial Connection Failed

```bash
# Check socket exists
ls -la /var/run/qemu-server/1002.serial0

# Test with socat
socat UNIX-CONNECT:/var/run/qemu-server/1002.serial0 STDIO
```

### Tests Timing Out

- Increase timeout: `--timeout 30`
- Check VM boot: Connect manually via serial
- Check for kernel panics in serial output

## Integration with CI

```bash
# CI script example
./run_all_tests.py --vm 1002 --output results.json --html-report
if [ $? -ne 0 ]; then
    echo "Tests failed!"
    exit 1
fi
```

## Requirements

- Python 3.8+
- Proxmox VE with `qm` commands
- Network/SSH access to Proxmox host
- MayteraOS VM with serial console enabled
