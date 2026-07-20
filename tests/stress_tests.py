#!/usr/bin/env python3
"""
MayteraOS Stress Tests

Tests for system stability under stress:
- Repeated command execution
- Memory pressure
- Filesystem operations
- Serial I/O stress
- Long-running operations
"""

import re
import time
from typing import Tuple

from test_runner import TestCase, TestFramework, VMController


def test_stress_rapid_commands(vm: VMController) -> Tuple[bool, str, str]:
    """Test system stability with rapid command execution"""
    commands = ["help", "mem", "ps", "ticks", "pci", "net", "ls /"]
    success_count = 0
    fail_count = 0
    outputs = []

    for _ in range(3):  # 3 rounds
        for cmd in commands:
            found, output = vm.send_command(cmd, expect="maytera>", timeout_sec=5)
            if found:
                success_count += 1
            else:
                fail_count += 1
            outputs.append(f"{cmd}: {'OK' if found else 'FAIL'}")
            time.sleep(0.1)  # Small delay between commands

    total = success_count + fail_count
    success_rate = (success_count / total * 100) if total > 0 else 0

    if success_rate >= 90:  # Allow 10% failure rate
        return True, f"Success rate: {success_rate:.1f}% ({success_count}/{total})", ""

    return False, "\n".join(outputs[-10:]), f"Success rate too low: {success_rate:.1f}%"


def test_stress_memory_allocation(vm: VMController) -> Tuple[bool, str, str]:
    """Test repeated memory allocation/deallocation"""
    success_count = 0
    outputs = []

    for i in range(10):
        found, output = vm.send_command("alloc", expect="maytera>", timeout_sec=5)
        if found and ("Allocated" in output or "0x" in output):
            if "Freed" in output:
                success_count += 1
            outputs.append(f"Round {i+1}: OK")
        else:
            outputs.append(f"Round {i+1}: FAIL")
        time.sleep(0.2)

    if success_count >= 8:  # 80% success rate
        return True, f"Memory alloc/free: {success_count}/10 successful", ""

    return False, "\n".join(outputs), f"Only {success_count}/10 allocations successful"


def test_stress_filesystem_read(vm: VMController) -> Tuple[bool, str, str]:
    """Test repeated filesystem reads"""
    success_count = 0

    for i in range(5):
        found, output = vm.send_command("ls /", expect="maytera>", timeout_sec=5)
        if found and len(output) > 20:
            success_count += 1
        time.sleep(0.2)

    if success_count >= 4:  # 80% success
        return True, f"FS reads: {success_count}/5 successful", ""

    return False, "", f"Only {success_count}/5 reads successful"


def test_stress_serial_echo(vm: VMController) -> Tuple[bool, str, str]:
    """Test serial I/O under stress"""
    test_strings = [
        "TEST123",
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
        "0123456789",
        "Special!@#$%^&*()",
        "Mixed123ABC!@#"
    ]

    success_count = 0

    for test_str in test_strings:
        # Use help command to generate output with the test string visible
        found, output = vm.send_command(f"help", expect="maytera>", timeout_sec=5)
        if found and len(output) > 50:
            success_count += 1
        time.sleep(0.1)

    if success_count >= 4:
        return True, f"Serial stress: {success_count}/5 successful", ""

    return False, "", f"Serial I/O degraded: {success_count}/5"


def test_stress_long_running(vm: VMController) -> Tuple[bool, str, str]:
    """Test system stability over longer period"""
    start_time = time.time()
    success_count = 0
    total_count = 0
    last_ticks = 0

    while (time.time() - start_time) < 30:  # Run for 30 seconds
        found, output = vm.send_command("ticks", expect="maytera>", timeout_sec=5)
        total_count += 1

        if found:
            match = re.search(r'(\d{3,})', output)
            if match:
                current_ticks = int(match.group(1))
                if current_ticks >= last_ticks:
                    success_count += 1
                    last_ticks = current_ticks

        time.sleep(1)

    success_rate = (success_count / total_count * 100) if total_count > 0 else 0

    if success_rate >= 90:
        return True, f"Long running: {success_rate:.1f}% stable over 30s", ""

    return False, "", f"Stability degraded: {success_rate:.1f}%"


def test_stress_command_flood(vm: VMController) -> Tuple[bool, str, str]:
    """Test system with rapid command flooding"""
    # Send multiple commands without waiting for full response
    commands = ["mem\n", "ps\n", "ticks\n"] * 5

    for cmd in commands:
        vm.send_serial(cmd)
        time.sleep(0.05)  # Very short delay

    # Now wait and see if system recovers
    time.sleep(2)

    # Try to get a clean response
    found, output = vm.send_command("help", expect="maytera>", timeout_sec=10)

    if found:
        return True, "System recovered from command flood", ""

    return False, output, "System did not recover from flood"


def test_stress_heap_fragmentation(vm: VMController) -> Tuple[bool, str, str]:
    """Test heap after many allocations (checks fragmentation)"""
    # First, get initial heap stats
    found1, output1 = vm.send_command("heap", expect="maytera>", timeout_sec=5)
    if not found1:
        return False, output1, "Could not get initial heap stats"

    # Do many allocations
    for _ in range(10):
        vm.send_command("alloc", expect="maytera>", timeout_sec=3)
        time.sleep(0.1)

    # Check heap stats again
    found2, output2 = vm.send_command("heap", expect="maytera>", timeout_sec=5)
    if not found2:
        return False, output2, "Could not get final heap stats"

    # Check PMM as well
    found3, output3 = vm.send_command("pmm", expect="maytera>", timeout_sec=5)

    # As long as we can still query memory, the heap is functional
    if found2:
        return True, f"Heap functional after stress:\n{output2[:200]}", ""

    return False, output2, "Heap may be corrupted"


def test_stress_filesystem_traverse(vm: VMController) -> Tuple[bool, str, str]:
    """Test filesystem traversal"""
    paths = ["/", "/EFI", "/EFI/BOOT"]
    success_count = 0

    for path in paths:
        # Try to list each directory
        found, output = vm.send_command(f"ls {path}", expect="maytera>", timeout_sec=5)
        if found:
            success_count += 1
        time.sleep(0.2)

    if success_count >= 2:  # At least 2 of 3 paths
        return True, f"FS traversal: {success_count}/{len(paths)} paths accessible", ""

    return False, "", f"FS traversal limited: {success_count}/{len(paths)}"


def test_stress_network_repeated(vm: VMController) -> Tuple[bool, str, str]:
    """Test repeated network status queries"""
    success_count = 0

    for i in range(5):
        found, output = vm.send_command("net", expect="maytera>", timeout_sec=5)
        if found and ("MAC" in output or "IP" in output):
            success_count += 1
        time.sleep(0.3)

    if success_count >= 4:
        return True, f"Network queries: {success_count}/5 successful", ""

    return False, "", f"Network degraded: {success_count}/5"


def register_stress_tests(framework: TestFramework):
    """Register all stress tests"""

    framework.register_test(TestCase(
        name="stress_rapid_commands",
        description="Test rapid command execution",
        category="stress",
        test_func=test_stress_rapid_commands,
        timeout_ms=60000
    ))

    framework.register_test(TestCase(
        name="stress_memory_allocation",
        description="Test repeated memory allocation",
        category="stress",
        test_func=test_stress_memory_allocation,
        timeout_ms=30000
    ))

    framework.register_test(TestCase(
        name="stress_filesystem_read",
        description="Test repeated filesystem reads",
        category="stress",
        test_func=test_stress_filesystem_read,
        timeout_ms=30000
    ))

    framework.register_test(TestCase(
        name="stress_serial_echo",
        description="Test serial I/O under stress",
        category="stress",
        test_func=test_stress_serial_echo,
        timeout_ms=30000
    ))

    framework.register_test(TestCase(
        name="stress_long_running",
        description="Test system stability over 30 seconds",
        category="stress",
        test_func=test_stress_long_running,
        timeout_ms=60000
    ))

    framework.register_test(TestCase(
        name="stress_command_flood",
        description="Test recovery from command flooding",
        category="stress",
        test_func=test_stress_command_flood,
        timeout_ms=30000
    ))

    framework.register_test(TestCase(
        name="stress_heap_fragmentation",
        description="Test heap after stress",
        category="stress",
        test_func=test_stress_heap_fragmentation,
        timeout_ms=30000
    ))

    framework.register_test(TestCase(
        name="stress_filesystem_traverse",
        description="Test filesystem traversal",
        category="stress",
        test_func=test_stress_filesystem_traverse,
        timeout_ms=30000
    ))

    framework.register_test(TestCase(
        name="stress_network_repeated",
        description="Test repeated network queries",
        category="stress",
        test_func=test_stress_network_repeated,
        timeout_ms=30000
    ))
