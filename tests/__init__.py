"""
MayteraOS Automated Testing Framework

This package provides automated testing capabilities for MayteraOS.
"""

from .test_runner import (
    TestCase,
    TestFramework,
    TestOutcome,
    TestResult,
    TestSuiteResult,
    VMController,
    create_default_tests,
)

from .syscall_tests import register_syscall_tests
from .ipc_tests import register_ipc_tests
from .stress_tests import register_stress_tests
from .capability_tests import register_capability_tests

__version__ = "1.0.0"
__all__ = [
    "TestCase",
    "TestFramework",
    "TestOutcome",
    "TestResult",
    "TestSuiteResult",
    "VMController",
    "create_default_tests",
    "register_syscall_tests",
    "register_ipc_tests",
    "register_stress_tests",
    "register_capability_tests",
]
