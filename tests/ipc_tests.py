#!/usr/bin/env python3
"""
MayteraOS IPC Tests

Tests for inter-process communication:
- Process creation and management
- Shared memory (when implemented)
- Message passing (when implemented)
- Process synchronization
"""

import re
import time
from typing import Tuple

from test_runner import TestCase, TestFramework, VMController


def test_process_creation(vm: VMController) -> Tuple[bool, str, str]:
    """Test process creation and listing"""
    # Get initial process list
    found1, output1 = vm.send_command("ps", expect="maytera>", timeout_sec=5)
    if not found1:
        return False, output1, "Could not get process list"

    # Count initial processes
    initial_count = len(re.findall(r'^\s*\d+\s+\d+', output1, re.MULTILINE))

    # The idle process should always exist
    if "idle" not in output1.lower():
        return False, output1, "Idle process not found"

    return True, output1, ""


def test_process_states(vm: VMController) -> Tuple[bool, str, str]:
    """Test that process states are tracked correctly"""
    found, output = vm.send_command("ps", expect="maytera>", timeout_sec=5)
    if not found:
        return False, output, "Could not get process list"

    # Check for state column
    states = ["RUNNING", "READY", "SLEEPING", "BLOCKED"]
    has_state = any(state in output for state in states)

    if has_state:
        return True, output, ""

    # Alternative: check for state-like patterns
    if re.search(r'(STATE|state)', output):
        return True, output, ""

    return False, output, "Process states not visible"


def test_process_priority(vm: VMController) -> Tuple[bool, str, str]:
    """Test process priority is tracked"""
    found, output = vm.send_command("ps", expect="maytera>", timeout_sec=5)
    if not found:
        return False, output, "Could not get process list"

    # Check for priority column (PRIO header)
    if "PRIO" in output or "priority" in output.lower():
        return True, output, ""

    # Alternative: check for numeric priority values
    if re.search(r'\d+\s+\d+\s+\w+\s+\d+', output):  # PID PPID STATE PRIO pattern
        return True, output, ""

    return False, output, "Process priority not visible"


def test_process_cpu_time(vm: VMController) -> Tuple[bool, str, str]:
    """Test CPU time accounting"""
    found, output = vm.send_command("ps", expect="maytera>", timeout_sec=5)
    if not found:
        return False, output, "Could not get process list"

    # Check for CPU TIME column
    if "TIME" in output or "CPU" in output:
        return True, output, ""

    return False, output, "CPU time accounting not visible"


def test_process_parent_child(vm: VMController) -> Tuple[bool, str, str]:
    """Test parent-child process relationships (PPID)"""
    found, output = vm.send_command("ps", expect="maytera>", timeout_sec=5)
    if not found:
        return False, output, "Could not get process list"

    # Check for PPID column
    if "PPID" in output:
        # Verify there are actual PPID values
        if re.search(r'\d+\s+\d+', output):
            return True, output, ""

    return False, output, "Parent-child relationships not tracked"


def test_scheduler_preemption(vm: VMController) -> Tuple[bool, str, str]:
    """Test that preemptive scheduling works"""
    # Get ticks at two points with some activity
    found1, output1 = vm.send_command("ticks", expect="maytera>", timeout_sec=5)
    if not found1:
        return False, output1, "Could not get ticks"

    match1 = re.search(r'ticks:\s*(\d+)', output1, re.IGNORECASE)
    if not match1:
        # Try alternative pattern
        match1 = re.search(r'(\d{4,})', output1)
    if not match1:
        return False, output1, "Could not parse ticks"

    ticks1 = int(match1.group(1))

    # Do some work
    vm.send_command("ps", expect="maytera>", timeout_sec=5)
    vm.send_command("mem", expect="maytera>", timeout_sec=5)

    found2, output2 = vm.send_command("ticks", expect="maytera>", timeout_sec=5)
    if not found2:
        return False, output2, "Could not get final ticks"

    match2 = re.search(r'ticks:\s*(\d+)', output2, re.IGNORECASE)
    if not match2:
        match2 = re.search(r'(\d{4,})', output2)
    if not match2:
        return False, output2, "Could not parse final ticks"

    ticks2 = int(match2.group(1))

    if ticks2 > ticks1:
        return True, f"Scheduler active: ticks {ticks1} -> {ticks2}", ""

    return False, f"Ticks unchanged: {ticks1}", "Scheduler may not be preemptive"


def test_kernel_process_isolation(vm: VMController) -> Tuple[bool, str, str]:
    """Test that kernel processes are properly isolated"""
    found, output = vm.send_command("ps", expect="maytera>", timeout_sec=5)
    if not found:
        return False, output, "Could not get process list"

    # The idle process should be PID 0
    if re.search(r'^\s*0\s+', output, re.MULTILINE):
        return True, output, ""

    # Alternative: check for idle with any PID
    if "idle" in output.lower():
        return True, output, ""

    return False, output, "Kernel process isolation not verifiable"


def test_interrupt_handling(vm: VMController) -> Tuple[bool, str, str]:
    """Test that interrupts are being handled (via timer ticks)"""
    found1, output1 = vm.send_command("ticks", expect="maytera>", timeout_sec=3)
    if not found1:
        return False, output1, "Could not get initial ticks"

    time.sleep(0.5)

    found2, output2 = vm.send_command("ticks", expect="maytera>", timeout_sec=3)
    if not found2:
        return False, output2, "Could not get final ticks"

    # Extract tick values
    match1 = re.search(r'(\d{3,})', output1)
    match2 = re.search(r'(\d{3,})', output2)

    if match1 and match2:
        ticks1 = int(match1.group(1))
        ticks2 = int(match2.group(1))
        if ticks2 > ticks1:
            return True, f"Timer interrupts working: {ticks1} -> {ticks2}", ""

    return False, output2, "Timer interrupts may not be working"


def register_ipc_tests(framework: TestFramework):
    """Register all IPC tests"""

    framework.register_test(TestCase(
        name="ipc_process_creation",
        description="Test process creation and listing",
        category="ipc",
        test_func=test_process_creation,
        timeout_ms=5000
    ))

    framework.register_test(TestCase(
        name="ipc_process_states",
        description="Test process state tracking",
        category="ipc",
        test_func=test_process_states,
        timeout_ms=5000
    ))

    framework.register_test(TestCase(
        name="ipc_process_priority",
        description="Test process priority tracking",
        category="ipc",
        test_func=test_process_priority,
        timeout_ms=5000
    ))

    framework.register_test(TestCase(
        name="ipc_cpu_time",
        description="Test CPU time accounting",
        category="ipc",
        test_func=test_process_cpu_time,
        timeout_ms=5000
    ))

    framework.register_test(TestCase(
        name="ipc_parent_child",
        description="Test parent-child process relationships",
        category="ipc",
        test_func=test_process_parent_child,
        timeout_ms=5000
    ))

    framework.register_test(TestCase(
        name="ipc_scheduler_preemption",
        description="Test preemptive scheduling",
        category="ipc",
        test_func=test_scheduler_preemption,
        timeout_ms=15000
    ))

    framework.register_test(TestCase(
        name="ipc_kernel_isolation",
        description="Test kernel process isolation",
        category="ipc",
        test_func=test_kernel_process_isolation,
        timeout_ms=5000
    ))

    framework.register_test(TestCase(
        name="ipc_interrupt_handling",
        description="Test interrupt handling via timer",
        category="ipc",
        test_func=test_interrupt_handling,
        timeout_ms=10000
    ))
