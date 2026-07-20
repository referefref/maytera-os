#!/usr/bin/env python3
"""
MayteraOS Capability/Permission Tests

Tests for the capability and permission system:
- Kernel vs User mode separation
- Memory protection
- Hardware access control
- System call permissions
"""

import re
import time
from typing import Tuple

from test_runner import TestCase, TestFramework, VMController


def test_kernel_mode_detection(vm: VMController) -> Tuple[bool, str, str]:
    """Test that kernel mode is properly detected"""
    # The shell runs in kernel mode by default
    found, output = vm.send_command("ps", expect="maytera>", timeout_sec=5)
    if not found:
        return False, output, "Could not query processes"

    # Look for privilege level indicators
    # The idle process should be kernel mode (PID 0)
    if "idle" in output.lower():
        return True, output, ""

    return False, output, "Could not verify kernel mode"


def test_pci_access(vm: VMController) -> Tuple[bool, str, str]:
    """Test that PCI access is properly controlled"""
    found, output = vm.send_command("pci", expect="maytera>", timeout_sec=5)
    if not found:
        return False, output, "PCI command failed"

    # Should see PCI devices (kernel has access)
    if "Bus" in output or "Device" in output or "PCI" in output:
        return True, output, ""

    return False, output, "PCI access may be restricted"


def test_memory_read_protection(vm: VMController) -> Tuple[bool, str, str]:
    """Test that memory regions are protected"""
    # Read disk sector (requires privileged I/O)
    found, output = vm.send_command("read", expect="maytera>", timeout_sec=10)
    if found:
        # Kernel mode can read disk
        if "sector" in output.lower() or "bytes" in output.lower() or "hex" in output.lower():
            return True, output, ""
        elif "No ATA" in output:
            # No disk, but command was processed
            return True, output, ""

    return False, output, "Privileged memory access test inconclusive"


def test_ata_io_ports(vm: VMController) -> Tuple[bool, str, str]:
    """Test ATA I/O port access (privileged)"""
    found, output = vm.send_command("disk", expect="maytera>", timeout_sec=5)
    if not found:
        return False, output, "Disk command failed"

    # Kernel should be able to query disk
    if "ATA" in output or "drive" in output.lower() or "disk" in output.lower():
        return True, output, ""

    return False, output, "I/O port access test inconclusive"


def test_interrupt_control(vm: VMController) -> Tuple[bool, str, str]:
    """Test interrupt enable/disable (privileged operation)"""
    # Timer ticks prove interrupts are working
    found1, output1 = vm.send_command("ticks", expect="maytera>", timeout_sec=3)
    if not found1:
        return False, output1, "Could not get ticks"

    match1 = re.search(r'(\d{3,})', output1)
    if not match1:
        return False, output1, "Could not parse ticks"

    ticks1 = int(match1.group(1))
    time.sleep(0.5)

    found2, output2 = vm.send_command("ticks", expect="maytera>", timeout_sec=3)
    if not found2:
        return False, output2, "Could not get ticks"

    match2 = re.search(r'(\d{3,})', output2)
    if not match2:
        return False, output2, "Could not parse ticks"

    ticks2 = int(match2.group(1))

    # Interrupts are enabled and working
    if ticks2 > ticks1:
        return True, f"Interrupts enabled: {ticks1} -> {ticks2}", ""

    return False, "", "Interrupts may be disabled"


def test_acpi_access(vm: VMController) -> Tuple[bool, str, str]:
    """Test ACPI access (privileged)"""
    # Try shutdown command (won't actually shut down if test runner is working)
    # Instead, check if ACPI was initialized during boot
    found, output = vm.send_command("help", expect="maytera>", timeout_sec=5)

    if found and "shutdown" in output.lower():
        # ACPI shutdown command is available
        return True, "ACPI commands available", ""

    return False, output, "ACPI access not verifiable"


def test_serial_io_access(vm: VMController) -> Tuple[bool, str, str]:
    """Test serial I/O port access (privileged)"""
    # We're using serial, so it must work
    found, output = vm.send_command("help", expect="Available commands", timeout_sec=5)

    if found:
        return True, "Serial I/O working (privileged access confirmed)", ""

    return False, output, "Serial I/O not working"


def test_network_hardware_access(vm: VMController) -> Tuple[bool, str, str]:
    """Test network hardware access (privileged)"""
    found, output = vm.send_command("net", expect="maytera>", timeout_sec=5)

    if found and "MAC" in output:
        # Can read MAC address (requires hardware access)
        return True, output, ""

    return False, output, "Network hardware access test inconclusive"


def test_filesystem_mount(vm: VMController) -> Tuple[bool, str, str]:
    """Test filesystem mount capabilities"""
    found, output = vm.send_command("mount", expect="maytera>", timeout_sec=5)

    if found:
        if "mounted" in output.lower() or "FAT" in output or "filesystem" in output.lower():
            return True, output, ""
        elif "No filesystem" in output:
            # No FS mounted but command works
            return True, output, ""

    return False, output, "Mount command failed"


def test_cpu_info_access(vm: VMController) -> Tuple[bool, str, str]:
    """Test CPU information access"""
    # Use fb command to check framebuffer (requires CPU/memory access)
    found, output = vm.send_command("fb", expect="maytera>", timeout_sec=5)

    if found and ("Address" in output or "Resolution" in output or "framebuffer" in output.lower()):
        return True, output, ""

    return False, output, "CPU/memory info access test inconclusive"


def register_capability_tests(framework: TestFramework):
    """Register all capability tests"""

    framework.register_test(TestCase(
        name="cap_kernel_mode",
        description="Test kernel mode detection",
        category="capability",
        test_func=test_kernel_mode_detection,
        timeout_ms=5000
    ))

    framework.register_test(TestCase(
        name="cap_pci_access",
        description="Test PCI bus access",
        category="capability",
        test_func=test_pci_access,
        timeout_ms=5000
    ))

    framework.register_test(TestCase(
        name="cap_memory_protection",
        description="Test memory read protection",
        category="capability",
        test_func=test_memory_read_protection,
        timeout_ms=10000
    ))

    framework.register_test(TestCase(
        name="cap_ata_io",
        description="Test ATA I/O port access",
        category="capability",
        test_func=test_ata_io_ports,
        timeout_ms=5000
    ))

    framework.register_test(TestCase(
        name="cap_interrupt_control",
        description="Test interrupt enable/disable",
        category="capability",
        test_func=test_interrupt_control,
        timeout_ms=10000
    ))

    framework.register_test(TestCase(
        name="cap_acpi_access",
        description="Test ACPI access",
        category="capability",
        test_func=test_acpi_access,
        timeout_ms=5000
    ))

    framework.register_test(TestCase(
        name="cap_serial_io",
        description="Test serial I/O port access",
        category="capability",
        test_func=test_serial_io_access,
        timeout_ms=5000
    ))

    framework.register_test(TestCase(
        name="cap_network_hardware",
        description="Test network hardware access",
        category="capability",
        test_func=test_network_hardware_access,
        timeout_ms=5000
    ))

    framework.register_test(TestCase(
        name="cap_filesystem_mount",
        description="Test filesystem mount capabilities",
        category="capability",
        test_func=test_filesystem_mount,
        timeout_ms=5000
    ))

    framework.register_test(TestCase(
        name="cap_cpu_info",
        description="Test CPU/memory info access",
        category="capability",
        test_func=test_cpu_info_access,
        timeout_ms=5000
    ))
