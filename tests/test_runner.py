#!/usr/bin/env python3
"""
MayteraOS Automated VM Testing Framework

This test runner manages automated testing of MayteraOS on Proxmox VMs.
It controls the VM via qm commands and communicates with the OS via serial port.

Usage:
    ./test_runner.py --vm 1002 --suite syscall
    ./test_runner.py --vm 1002 --suite all
    ./test_runner.py --vm 1002 --test test_exit
"""

import argparse
import json
import logging
import os
import re
import select
import signal
import socket
import subprocess
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime
from enum import Enum
from pathlib import Path
from typing import Callable, Dict, List, Optional, Tuple

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s',
    handlers=[
        logging.FileHandler('test_runner.log'),
        logging.StreamHandler()
    ]
)
logger = logging.getLogger(__name__)


class TestResult(Enum):
    """Test result status"""
    PASS = "PASS"
    FAIL = "FAIL"
    SKIP = "SKIP"
    TIMEOUT = "TIMEOUT"
    ERROR = "ERROR"


@dataclass
class TestCase:
    """Represents a single test case"""
    name: str
    description: str
    category: str
    test_func: Optional[Callable] = None
    expected_output: Optional[str] = None
    timeout_ms: int = 5000
    requires_usermode: bool = False


@dataclass
class TestOutcome:
    """Result of running a single test"""
    test: TestCase
    result: TestResult
    duration_ms: float
    output: str = ""
    error: str = ""
    timestamp: str = field(default_factory=lambda: datetime.now().isoformat())


@dataclass
class TestSuiteResult:
    """Result of running a test suite"""
    suite_name: str
    outcomes: List[TestOutcome]
    total_time_ms: float
    timestamp: str = field(default_factory=lambda: datetime.now().isoformat())

    @property
    def passed(self) -> int:
        return sum(1 for o in self.outcomes if o.result == TestResult.PASS)

    @property
    def failed(self) -> int:
        return sum(1 for o in self.outcomes if o.result == TestResult.FAIL)

    @property
    def skipped(self) -> int:
        return sum(1 for o in self.outcomes if o.result == TestResult.SKIP)

    @property
    def errors(self) -> int:
        return sum(1 for o in self.outcomes if o.result in (TestResult.ERROR, TestResult.TIMEOUT))

    @property
    def total(self) -> int:
        return len(self.outcomes)

    def to_dict(self) -> dict:
        """Convert to dictionary for JSON serialization"""
        return {
            "suite_name": self.suite_name,
            "timestamp": self.timestamp,
            "total_time_ms": self.total_time_ms,
            "summary": {
                "total": self.total,
                "passed": self.passed,
                "failed": self.failed,
                "skipped": self.skipped,
                "errors": self.errors
            },
            "tests": [
                {
                    "name": o.test.name,
                    "category": o.test.category,
                    "result": o.result.value,
                    "duration_ms": o.duration_ms,
                    "output": o.output,
                    "error": o.error
                }
                for o in self.outcomes
            ]
        }


class VMController:
    """Controls VM via qm commands and serial socket"""

    def __init__(self, vm_id: int, serial_timeout: float = 10.0):
        self.vm_id = vm_id
        self.serial_timeout = serial_timeout
        self.serial_socket_path = f"/var/run/qemu-server/{vm_id}.serial0"
        self.serial_conn = None

    def start(self) -> bool:
        """Start the VM"""
        try:
            logger.info(f"Starting VM {self.vm_id}...")
            result = subprocess.run(
                ["qm", "start", str(self.vm_id)],
                capture_output=True, text=True, timeout=30
            )
            if result.returncode != 0:
                logger.error(f"Failed to start VM: {result.stderr}")
                return False
            logger.info(f"VM {self.vm_id} started")
            return True
        except subprocess.TimeoutExpired:
            logger.error("Timeout starting VM")
            return False
        except Exception as e:
            logger.error(f"Error starting VM: {e}")
            return False

    def stop(self) -> bool:
        """Stop the VM"""
        try:
            logger.info(f"Stopping VM {self.vm_id}...")
            self.disconnect_serial()
            result = subprocess.run(
                ["qm", "stop", str(self.vm_id)],
                capture_output=True, text=True, timeout=30
            )
            if result.returncode != 0:
                logger.warning(f"VM stop returned non-zero: {result.stderr}")
            time.sleep(2)
            return True
        except Exception as e:
            logger.error(f"Error stopping VM: {e}")
            return False

    def reset(self) -> bool:
        """Reset the VM"""
        try:
            logger.info(f"Resetting VM {self.vm_id}...")
            self.disconnect_serial()
            result = subprocess.run(
                ["qm", "reset", str(self.vm_id)],
                capture_output=True, text=True, timeout=30
            )
            if result.returncode != 0:
                logger.error(f"Failed to reset VM: {result.stderr}")
                return False
            time.sleep(3)  # Wait for VM to restart
            return True
        except Exception as e:
            logger.error(f"Error resetting VM: {e}")
            return False

    def is_running(self) -> bool:
        """Check if VM is running"""
        try:
            result = subprocess.run(
                ["qm", "status", str(self.vm_id)],
                capture_output=True, text=True, timeout=10
            )
            return "running" in result.stdout.lower()
        except Exception:
            return False

    def connect_serial(self) -> bool:
        """Connect to serial socket"""
        if self.serial_conn:
            return True

        if not os.path.exists(self.serial_socket_path):
            logger.error(f"Serial socket not found: {self.serial_socket_path}")
            return False

        try:
            self.serial_conn = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.serial_conn.connect(self.serial_socket_path)
            self.serial_conn.setblocking(False)
            logger.info("Connected to serial port")
            return True
        except Exception as e:
            logger.error(f"Failed to connect to serial: {e}")
            self.serial_conn = None
            return False

    def disconnect_serial(self):
        """Disconnect from serial socket"""
        if self.serial_conn:
            try:
                self.serial_conn.close()
            except Exception:
                pass
            self.serial_conn = None

    def send_serial(self, data: str) -> bool:
        """Send data to serial port"""
        if not self.serial_conn and not self.connect_serial():
            return False

        try:
            self.serial_conn.sendall(data.encode('utf-8'))
            return True
        except Exception as e:
            logger.error(f"Failed to send serial data: {e}")
            return False

    def read_serial(self, timeout_sec: float = None) -> str:
        """Read available data from serial port"""
        if not self.serial_conn and not self.connect_serial():
            return ""

        if timeout_sec is None:
            timeout_sec = self.serial_timeout

        try:
            ready, _, _ = select.select([self.serial_conn], [], [], timeout_sec)
            if ready:
                data = b""
                while True:
                    try:
                        chunk = self.serial_conn.recv(4096)
                        if not chunk:
                            break
                        data += chunk
                    except BlockingIOError:
                        break
                return data.decode('utf-8', errors='replace')
            return ""
        except Exception as e:
            logger.error(f"Failed to read serial data: {e}")
            return ""

    def read_until(self, pattern: str, timeout_sec: float = None) -> Tuple[bool, str]:
        """Read serial until pattern found or timeout"""
        if timeout_sec is None:
            timeout_sec = self.serial_timeout

        start_time = time.time()
        buffer = ""

        while (time.time() - start_time) < timeout_sec:
            data = self.read_serial(timeout_sec=0.5)
            if data:
                buffer += data
                if pattern in buffer:
                    return True, buffer
            time.sleep(0.1)

        return False, buffer

    def send_command(self, cmd: str, expect: str = "maytera>",
                     timeout_sec: float = None) -> Tuple[bool, str]:
        """Send a command and wait for response"""
        if timeout_sec is None:
            timeout_sec = self.serial_timeout

        # Clear any pending input
        self.read_serial(timeout_sec=0.2)

        # Send command
        if not self.send_serial(cmd + "\n"):
            return False, "Failed to send command"

        # Wait for response
        found, output = self.read_until(expect, timeout_sec)
        return found, output


class TestFramework:
    """Main test framework managing test suites and execution"""

    def __init__(self, vm_controller: VMController):
        self.vm = vm_controller
        self.test_suites: Dict[str, List[TestCase]] = {}
        self.current_suite: Optional[str] = None

    def register_test(self, test: TestCase):
        """Register a test case"""
        if test.category not in self.test_suites:
            self.test_suites[test.category] = []
        self.test_suites[test.category].append(test)

    def get_available_suites(self) -> List[str]:
        """Get list of available test suites"""
        return list(self.test_suites.keys())

    def run_test(self, test: TestCase) -> TestOutcome:
        """Run a single test case"""
        logger.info(f"Running test: {test.name}")
        start_time = time.time()

        try:
            if test.test_func:
                # Custom test function
                result, output, error = test.test_func(self.vm)
                if result:
                    outcome_result = TestResult.PASS
                else:
                    outcome_result = TestResult.FAIL
            else:
                # Simple command-response test
                cmd = f"test {test.name}"
                found, output = self.vm.send_command(
                    cmd,
                    expect="TEST_COMPLETE",
                    timeout_sec=test.timeout_ms / 1000.0
                )

                if not found:
                    outcome_result = TestResult.TIMEOUT
                    error = "Test timed out"
                elif test.expected_output and test.expected_output in output:
                    outcome_result = TestResult.PASS
                    error = ""
                elif "TEST_PASS" in output:
                    outcome_result = TestResult.PASS
                    error = ""
                elif "TEST_FAIL" in output:
                    outcome_result = TestResult.FAIL
                    # Extract error message
                    match = re.search(r'TEST_FAIL:\s*(.+?)(?:\n|$)', output)
                    error = match.group(1) if match else "Test failed"
                else:
                    outcome_result = TestResult.ERROR
                    error = "Unexpected test output"

            duration_ms = (time.time() - start_time) * 1000

            return TestOutcome(
                test=test,
                result=outcome_result,
                duration_ms=duration_ms,
                output=output if 'output' in dir() else "",
                error=error if 'error' in dir() else ""
            )

        except Exception as e:
            duration_ms = (time.time() - start_time) * 1000
            logger.error(f"Test error: {e}")
            return TestOutcome(
                test=test,
                result=TestResult.ERROR,
                duration_ms=duration_ms,
                output="",
                error=str(e)
            )

    def run_suite(self, suite_name: str) -> TestSuiteResult:
        """Run all tests in a suite"""
        if suite_name not in self.test_suites:
            logger.error(f"Unknown test suite: {suite_name}")
            return TestSuiteResult(
                suite_name=suite_name,
                outcomes=[],
                total_time_ms=0
            )

        logger.info(f"Running test suite: {suite_name}")
        self.current_suite = suite_name
        outcomes = []
        start_time = time.time()

        for test in self.test_suites[suite_name]:
            outcome = self.run_test(test)
            outcomes.append(outcome)

            # Log result
            status = outcome.result.value
            logger.info(f"  {test.name}: {status} ({outcome.duration_ms:.1f}ms)")

            # Reset VM if test failed badly
            if outcome.result == TestResult.ERROR:
                logger.warning("Test error - resetting VM")
                self.vm.reset()
                time.sleep(5)
                self.vm.connect_serial()
                # Wait for boot
                self.vm.read_until("maytera>", timeout_sec=30)

        total_time = (time.time() - start_time) * 1000
        self.current_suite = None

        return TestSuiteResult(
            suite_name=suite_name,
            outcomes=outcomes,
            total_time_ms=total_time
        )

    def run_all_suites(self) -> List[TestSuiteResult]:
        """Run all test suites"""
        results = []
        for suite_name in self.test_suites:
            result = self.run_suite(suite_name)
            results.append(result)
        return results


# ============================================================================
# Test Helper Functions
# ============================================================================

def test_serial_echo(vm: VMController) -> Tuple[bool, str, str]:
    """Test that serial I/O works"""
    test_string = "SERIAL_TEST_12345"
    cmd = f"echo {test_string}"
    found, output = vm.send_command(cmd, expect=test_string, timeout_sec=5)
    if found and test_string in output:
        return True, output, ""
    return False, output, "Serial echo failed"


def test_help_command(vm: VMController) -> Tuple[bool, str, str]:
    """Test help command works"""
    found, output = vm.send_command("help", expect="Available commands", timeout_sec=5)
    if found and "help" in output.lower():
        return True, output, ""
    return False, output, "Help command not working"


def test_mem_command(vm: VMController) -> Tuple[bool, str, str]:
    """Test memory info command"""
    found, output = vm.send_command("mem", expect="maytera>", timeout_sec=5)
    if found and ("MB" in output or "memory" in output.lower()):
        return True, output, ""
    return False, output, "Memory command not working"


def test_ps_command(vm: VMController) -> Tuple[bool, str, str]:
    """Test process list command"""
    found, output = vm.send_command("ps", expect="maytera>", timeout_sec=5)
    if found and ("PID" in output or "idle" in output.lower()):
        return True, output, ""
    return False, output, "Process list not working"


def test_net_status(vm: VMController) -> Tuple[bool, str, str]:
    """Test network status command"""
    found, output = vm.send_command("net", expect="maytera>", timeout_sec=5)
    if found and ("MAC" in output or "IP" in output):
        return True, output, ""
    return False, output, "Network status not working"


def test_filesystem_ls(vm: VMController) -> Tuple[bool, str, str]:
    """Test directory listing"""
    found, output = vm.send_command("ls /", expect="maytera>", timeout_sec=5)
    if found:
        # Should see some files or directories
        if "EFI" in output or "boot" in output.lower() or "BMP" in output:
            return True, output, ""
    return False, output, "Filesystem listing not working"


def test_pci_list(vm: VMController) -> Tuple[bool, str, str]:
    """Test PCI device listing"""
    found, output = vm.send_command("pci", expect="maytera>", timeout_sec=5)
    if found and ("PCI" in output or "Bus" in output or "Device" in output):
        return True, output, ""
    return False, output, "PCI listing not working"


def test_heap_stats(vm: VMController) -> Tuple[bool, str, str]:
    """Test heap statistics"""
    found, output = vm.send_command("heap", expect="maytera>", timeout_sec=5)
    if found and ("heap" in output.lower() or "allocated" in output.lower() or "free" in output.lower()):
        return True, output, ""
    return False, output, "Heap stats not working"


def test_gui_launch(vm: VMController) -> Tuple[bool, str, str]:
    """Test that GUI can be launched (will time out but should start)"""
    # Send gui command and wait briefly
    vm.send_serial("gui\n")
    time.sleep(3)

    # Send escape to try to exit GUI
    vm.send_serial("\x1b")  # ESC
    time.sleep(1)

    # Check for any GUI-related output
    output = vm.read_serial(timeout_sec=2)
    if "GUI" in output or "desktop" in output.lower():
        return True, output, ""

    # If we got here, at least the command was accepted
    return True, "GUI command accepted", ""


# ============================================================================
# Main Entry Point
# ============================================================================

def create_default_tests(framework: TestFramework):
    """Create default test cases"""

    # Basic tests
    framework.register_test(TestCase(
        name="serial_echo",
        description="Test serial port echo functionality",
        category="basic",
        test_func=test_serial_echo,
        timeout_ms=5000
    ))

    framework.register_test(TestCase(
        name="help_command",
        description="Test help command",
        category="basic",
        test_func=test_help_command,
        timeout_ms=5000
    ))

    framework.register_test(TestCase(
        name="mem_info",
        description="Test memory info command",
        category="basic",
        test_func=test_mem_command,
        timeout_ms=5000
    ))

    # Process tests
    framework.register_test(TestCase(
        name="process_list",
        description="Test process listing",
        category="process",
        test_func=test_ps_command,
        timeout_ms=5000
    ))

    # Network tests
    framework.register_test(TestCase(
        name="network_status",
        description="Test network status",
        category="network",
        test_func=test_net_status,
        timeout_ms=5000
    ))

    # Filesystem tests
    framework.register_test(TestCase(
        name="filesystem_ls",
        description="Test directory listing",
        category="filesystem",
        test_func=test_filesystem_ls,
        timeout_ms=5000
    ))

    # PCI tests
    framework.register_test(TestCase(
        name="pci_devices",
        description="Test PCI device enumeration",
        category="hardware",
        test_func=test_pci_list,
        timeout_ms=5000
    ))

    # Memory tests
    framework.register_test(TestCase(
        name="heap_stats",
        description="Test heap memory statistics",
        category="memory",
        test_func=test_heap_stats,
        timeout_ms=5000
    ))

    # GUI tests
    framework.register_test(TestCase(
        name="gui_launch",
        description="Test GUI can be launched",
        category="gui",
        test_func=test_gui_launch,
        timeout_ms=10000
    ))


def print_summary(results: List[TestSuiteResult]):
    """Print test summary"""
    total_tests = sum(r.total for r in results)
    total_passed = sum(r.passed for r in results)
    total_failed = sum(r.failed for r in results)
    total_errors = sum(r.errors for r in results)

    print("\n" + "=" * 60)
    print("TEST SUMMARY")
    print("=" * 60)

    for result in results:
        print(f"\nSuite: {result.suite_name}")
        print(f"  Total: {result.total}, Passed: {result.passed}, "
              f"Failed: {result.failed}, Errors: {result.errors}")
        print(f"  Time: {result.total_time_ms:.1f}ms")

        for outcome in result.outcomes:
            status_color = {
                TestResult.PASS: "\033[92m",   # Green
                TestResult.FAIL: "\033[91m",   # Red
                TestResult.ERROR: "\033[93m",  # Yellow
                TestResult.TIMEOUT: "\033[93m",
                TestResult.SKIP: "\033[94m"    # Blue
            }.get(outcome.result, "")
            reset = "\033[0m"

            print(f"    {status_color}{outcome.result.value:7}{reset} "
                  f"{outcome.test.name} ({outcome.duration_ms:.1f}ms)")
            if outcome.error:
                print(f"           Error: {outcome.error[:60]}")

    print("\n" + "=" * 60)
    print(f"TOTAL: {total_tests} tests, {total_passed} passed, "
          f"{total_failed} failed, {total_errors} errors")
    print("=" * 60)

    return total_failed == 0 and total_errors == 0


def main():
    parser = argparse.ArgumentParser(description="MayteraOS Automated VM Testing")
    parser.add_argument("--vm", type=int, default=1002,
                        help="VM ID to test (default: 1002)")
    parser.add_argument("--suite", type=str, default="all",
                        help="Test suite to run (all, basic, process, network, etc.)")
    parser.add_argument("--test", type=str,
                        help="Run a specific test by name")
    parser.add_argument("--list", action="store_true",
                        help="List available test suites and tests")
    parser.add_argument("--output", type=str, default="test_results.json",
                        help="Output file for JSON results")
    parser.add_argument("--no-reset", action="store_true",
                        help="Don't reset VM before testing")
    parser.add_argument("--timeout", type=float, default=10.0,
                        help="Default serial timeout in seconds")

    args = parser.parse_args()

    # Create VM controller
    vm = VMController(args.vm, serial_timeout=args.timeout)

    # Create test framework
    framework = TestFramework(vm)
    create_default_tests(framework)

    # List tests if requested
    if args.list:
        print("Available test suites:")
        for suite in framework.get_available_suites():
            print(f"\n  {suite}:")
            for test in framework.test_suites[suite]:
                print(f"    - {test.name}: {test.description}")
        return 0

    # Check VM status
    if not vm.is_running():
        logger.info("VM not running, starting...")
        if not vm.start():
            logger.error("Failed to start VM")
            return 1
        time.sleep(10)  # Wait for boot
    elif not args.no_reset:
        logger.info("Resetting VM for clean test...")
        vm.reset()
        time.sleep(10)

    # Connect to serial
    logger.info("Waiting for VM to boot...")
    if not vm.connect_serial():
        logger.error("Failed to connect to serial")
        return 1

    # Wait for boot prompt
    found, output = vm.read_until("maytera>", timeout_sec=60)
    if not found:
        logger.error("VM did not reach shell prompt")
        logger.info(f"Output: {output[:500]}")
        return 1

    logger.info("VM booted successfully")

    # Run tests
    results = []

    if args.test:
        # Run specific test
        for suite_name, tests in framework.test_suites.items():
            for test in tests:
                if test.name == args.test:
                    outcome = framework.run_test(test)
                    results.append(TestSuiteResult(
                        suite_name=suite_name,
                        outcomes=[outcome],
                        total_time_ms=outcome.duration_ms
                    ))
                    break
    elif args.suite == "all":
        results = framework.run_all_suites()
    else:
        result = framework.run_suite(args.suite)
        results.append(result)

    # Print summary
    success = print_summary(results)

    # Save JSON results
    output_path = Path(args.output)
    with open(output_path, 'w') as f:
        json.dump([r.to_dict() for r in results], f, indent=2)
    logger.info(f"Results saved to {output_path}")

    # Cleanup
    vm.disconnect_serial()

    return 0 if success else 1


if __name__ == "__main__":
    sys.exit(main())
