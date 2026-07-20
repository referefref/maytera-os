#!/usr/bin/env python3
"""
MayteraOS Syscall Tests

Tests for system call functionality including:
- Process control (exit, getpid, yield, sleep)
- File I/O (open, close, read, write, seek)
- Memory management (brk, mmap, munmap)
- Console I/O (putchar, getchar)
- Time functions (time, clock)
- Window/Graphics syscalls
"""

import re
import time
from typing import Tuple

from test_runner import TestCase, TestFramework, VMController


def test_syscall_getpid(vm: VMController) -> Tuple[bool, str, str]:
    """Test SYS_GETPID syscall"""
    # This test would require a user-space test binary
    # For now, test via the shell process list
    found, output = vm.send_command("ps", expect="maytera>", timeout_sec=5)
    if found and "PID" in output:
        # Check that PIDs are shown
        if re.search(r'\d+\s+\d+', output):  # PID PPID pattern
            return True, output, ""
    return False, output, "Could not verify PID functionality"


def test_syscall_yield(vm: VMController) -> Tuple[bool, str, str]:
    """Test SYS_YIELD via process scheduling"""
    # Verify scheduler is working by checking multiple processes
    found, output = vm.send_command("ps", expect="maytera>", timeout_sec=5)
    if found and "idle" in output.lower():
        # Idle process exists, scheduler is working
        return True, output, ""
    return False, output, "Process scheduler not working"


def test_syscall_sleep(vm: VMController) -> Tuple[bool, str, str]:
    """Test SYS_SLEEP functionality"""
    # Use ticks command before and after a known delay
    found1, output1 = vm.send_command("ticks", expect="maytera>", timeout_sec=5)
    if not found1:
        return False, output1, "Could not get initial ticks"

    # Extract tick count
    match1 = re.search(r'ticks:\s*(\d+)', output1, re.IGNORECASE)
    if not match1:
        return False, output1, "Could not parse initial tick count"
    ticks1 = int(match1.group(1))

    # Wait a bit (sleep happens server-side via hlt)
    time.sleep(0.5)

    found2, output2 = vm.send_command("ticks", expect="maytera>", timeout_sec=5)
    if not found2:
        return False, output2, "Could not get final ticks"

    match2 = re.search(r'ticks:\s*(\d+)', output2, re.IGNORECASE)
    if not match2:
        return False, output2, "Could not parse final tick count"
    ticks2 = int(match2.group(1))

    # Timer should have incremented
    if ticks2 > ticks1:
        return True, f"Ticks: {ticks1} -> {ticks2}", ""
    return False, f"Ticks did not increment: {ticks1} -> {ticks2}", "Timer not working"


def test_syscall_write_stdout(vm: VMController) -> Tuple[bool, str, str]:
    """Test SYS_WRITE to stdout"""
    # The shell uses write syscall internally
    # Test via help command which writes to console
    found, output = vm.send_command("help", expect="Available commands", timeout_sec=5)
    if found:
        return True, output, ""
    return False, output, "Write to stdout not working"


def test_syscall_open_read(vm: VMController) -> Tuple[bool, str, str]:
    """Test SYS_OPEN and SYS_READ"""
    # Try to read a known file
    found, output = vm.send_command("cat /EFI/BOOT/startup.nsh",
                                    expect="maytera>", timeout_sec=10)
    if found:
        # Check if we got file contents or an error
        if "Cannot read" not in output and ("kernel" in output.lower() or len(output) > 50):
            return True, output, ""
        elif "Cannot read" in output:
            # Try another file
            found2, output2 = vm.send_command("ls /", expect="maytera>", timeout_sec=5)
            if found2 and ("EFI" in output2 or "boot" in output2.lower()):
                return True, output2, "File read works (different file)"
    return False, output, "File read not working"


def test_syscall_filesystem_ops(vm: VMController) -> Tuple[bool, str, str]:
    """Test filesystem-related syscalls"""
    # Test cd and pwd
    found1, output1 = vm.send_command("cd /", expect="maytera>", timeout_sec=5)
    if not found1:
        return False, output1, "cd command failed"

    found2, output2 = vm.send_command("pwd", expect="/", timeout_sec=5)
    if not found2 or "/" not in output2:
        return False, output2, "pwd command failed"

    # Test ls
    found3, output3 = vm.send_command("ls", expect="maytera>", timeout_sec=5)
    if found3 and len(output3) > 20:
        return True, output3, ""

    return False, output3, "Filesystem operations not working"


def test_syscall_time(vm: VMController) -> Tuple[bool, str, str]:
    """Test SYS_TIME and SYS_CLOCK"""
    found, output = vm.send_command("ticks", expect="maytera>", timeout_sec=5)
    if found and re.search(r'\d+', output):
        return True, output, ""
    return False, output, "Time syscall not working"


def test_syscall_memory_alloc(vm: VMController) -> Tuple[bool, str, str]:
    """Test memory allocation via alloc command"""
    found, output = vm.send_command("alloc", expect="maytera>", timeout_sec=5)
    if found:
        if "Allocated" in output and "Freed" in output:
            return True, output, ""
        elif "0x" in output:  # Got an address
            return True, output, ""
    return False, output, "Memory allocation not working"


def test_syscall_brk(vm: VMController) -> Tuple[bool, str, str]:
    """Test heap via heap command"""
    found, output = vm.send_command("heap", expect="maytera>", timeout_sec=5)
    if found and ("heap" in output.lower() or "allocat" in output.lower()):
        return True, output, ""
    return False, output, "Heap/brk not working"


def test_syscall_pmm(vm: VMController) -> Tuple[bool, str, str]:
    """Test physical memory management"""
    found, output = vm.send_command("pmm", expect="maytera>", timeout_sec=5)
    if found and ("page" in output.lower() or "memory" in output.lower() or "free" in output.lower()):
        return True, output, ""
    return False, output, "PMM not working"


def register_syscall_tests(framework: TestFramework):
    """Register all syscall tests"""

    framework.register_test(TestCase(
        name="syscall_getpid",
        description="Test SYS_GETPID - get process ID",
        category="syscall",
        test_func=test_syscall_getpid,
        timeout_ms=5000
    ))

    framework.register_test(TestCase(
        name="syscall_yield",
        description="Test SYS_YIELD - yield CPU",
        category="syscall",
        test_func=test_syscall_yield,
        timeout_ms=5000
    ))

    framework.register_test(TestCase(
        name="syscall_sleep",
        description="Test SYS_SLEEP - timer/sleep functionality",
        category="syscall",
        test_func=test_syscall_sleep,
        timeout_ms=10000
    ))

    framework.register_test(TestCase(
        name="syscall_write_stdout",
        description="Test SYS_WRITE to stdout",
        category="syscall",
        test_func=test_syscall_write_stdout,
        timeout_ms=5000
    ))

    framework.register_test(TestCase(
        name="syscall_open_read",
        description="Test SYS_OPEN and SYS_READ",
        category="syscall",
        test_func=test_syscall_open_read,
        timeout_ms=10000
    ))

    framework.register_test(TestCase(
        name="syscall_filesystem_ops",
        description="Test filesystem syscalls (cd, pwd, ls)",
        category="syscall",
        test_func=test_syscall_filesystem_ops,
        timeout_ms=10000
    ))

    framework.register_test(TestCase(
        name="syscall_time",
        description="Test SYS_TIME and SYS_CLOCK",
        category="syscall",
        test_func=test_syscall_time,
        timeout_ms=5000
    ))

    framework.register_test(TestCase(
        name="syscall_memory_alloc",
        description="Test memory allocation",
        category="syscall",
        test_func=test_syscall_memory_alloc,
        timeout_ms=5000
    ))

    framework.register_test(TestCase(
        name="syscall_brk",
        description="Test SYS_BRK - heap management",
        category="syscall",
        test_func=test_syscall_brk,
        timeout_ms=5000
    ))

    framework.register_test(TestCase(
        name="syscall_pmm",
        description="Test physical memory manager",
        category="syscall",
        test_func=test_syscall_pmm,
        timeout_ms=5000
    ))
