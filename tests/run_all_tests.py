#!/usr/bin/env python3
"""
MayteraOS Full Test Suite Runner

Runs all available test suites and generates comprehensive reports.

Usage:
    ./run_all_tests.py --vm 1002
    ./run_all_tests.py --vm 1002 --suite syscall
    ./run_all_tests.py --vm 1002 --html-report
"""

import argparse
import json
import logging
import os
import sys
import time
from datetime import datetime
from pathlib import Path

# Import base test framework
from test_runner import (
    TestFramework,
    VMController,
    TestSuiteResult,
    create_default_tests
)

# Import test modules
from syscall_tests import register_syscall_tests
from ipc_tests import register_ipc_tests
from stress_tests import register_stress_tests
from capability_tests import register_capability_tests

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s',
    handlers=[
        logging.FileHandler('test_runner_full.log'),
        logging.StreamHandler()
    ]
)
logger = logging.getLogger(__name__)


def generate_html_report(results: list, output_path: str):
    """Generate an HTML test report"""
    total_tests = sum(r.total for r in results)
    total_passed = sum(r.passed for r in results)
    total_failed = sum(r.failed for r in results)
    total_errors = sum(r.errors for r in results)

    pass_rate = (total_passed / total_tests * 100) if total_tests > 0 else 0

    html = f"""<!DOCTYPE html>
<html>
<head>
    <title>MayteraOS Test Report</title>
    <style>
        body {{
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            max-width: 1200px;
            margin: 0 auto;
            padding: 20px;
            background: #f5f5f5;
        }}
        h1 {{
            color: #333;
            border-bottom: 2px solid #007bff;
            padding-bottom: 10px;
        }}
        .summary {{
            background: white;
            padding: 20px;
            border-radius: 8px;
            box-shadow: 0 2px 4px rgba(0,0,0,0.1);
            margin-bottom: 20px;
        }}
        .summary h2 {{
            margin-top: 0;
        }}
        .stat {{
            display: inline-block;
            padding: 15px 25px;
            margin: 5px;
            border-radius: 4px;
            color: white;
            font-weight: bold;
        }}
        .stat-total {{ background: #6c757d; }}
        .stat-passed {{ background: #28a745; }}
        .stat-failed {{ background: #dc3545; }}
        .stat-errors {{ background: #ffc107; color: #333; }}
        .suite {{
            background: white;
            padding: 20px;
            border-radius: 8px;
            box-shadow: 0 2px 4px rgba(0,0,0,0.1);
            margin-bottom: 20px;
        }}
        .suite h3 {{
            margin-top: 0;
            border-bottom: 1px solid #ddd;
            padding-bottom: 10px;
        }}
        table {{
            width: 100%;
            border-collapse: collapse;
        }}
        th, td {{
            padding: 10px;
            text-align: left;
            border-bottom: 1px solid #ddd;
        }}
        th {{
            background: #f8f9fa;
        }}
        .result-PASS {{ color: #28a745; font-weight: bold; }}
        .result-FAIL {{ color: #dc3545; font-weight: bold; }}
        .result-ERROR {{ color: #ffc107; font-weight: bold; }}
        .result-TIMEOUT {{ color: #fd7e14; font-weight: bold; }}
        .result-SKIP {{ color: #6c757d; font-weight: bold; }}
        .error-msg {{
            color: #dc3545;
            font-size: 0.9em;
            margin-top: 5px;
        }}
        .progress-bar {{
            width: 100%;
            height: 30px;
            background: #e9ecef;
            border-radius: 4px;
            overflow: hidden;
        }}
        .progress-fill {{
            height: 100%;
            background: #28a745;
            transition: width 0.3s;
        }}
        .timestamp {{
            color: #6c757d;
            font-size: 0.9em;
        }}
    </style>
</head>
<body>
    <h1>MayteraOS Test Report</h1>
    <p class="timestamp">Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}</p>

    <div class="summary">
        <h2>Summary</h2>
        <div class="progress-bar">
            <div class="progress-fill" style="width: {pass_rate}%"></div>
        </div>
        <p style="text-align: center; font-size: 1.2em;">{pass_rate:.1f}% Pass Rate</p>
        <div style="text-align: center;">
            <span class="stat stat-total">Total: {total_tests}</span>
            <span class="stat stat-passed">Passed: {total_passed}</span>
            <span class="stat stat-failed">Failed: {total_failed}</span>
            <span class="stat stat-errors">Errors: {total_errors}</span>
        </div>
    </div>
"""

    for result in results:
        html += f"""
    <div class="suite">
        <h3>{result.suite_name}</h3>
        <p>Time: {result.total_time_ms:.1f}ms |
           Passed: {result.passed}/{result.total}</p>
        <table>
            <tr>
                <th>Test</th>
                <th>Result</th>
                <th>Duration</th>
                <th>Details</th>
            </tr>
"""
        for outcome in result.outcomes:
            error_html = f'<div class="error-msg">{outcome.error}</div>' if outcome.error else ''
            html += f"""
            <tr>
                <td><strong>{outcome.test.name}</strong><br>
                    <small>{outcome.test.description}</small></td>
                <td class="result-{outcome.result.value}">{outcome.result.value}</td>
                <td>{outcome.duration_ms:.1f}ms</td>
                <td>{error_html}</td>
            </tr>
"""
        html += """
        </table>
    </div>
"""

    html += """
</body>
</html>
"""

    with open(output_path, 'w') as f:
        f.write(html)

    logger.info(f"HTML report saved to {output_path}")


def print_summary(results: list) -> bool:
    """Print test summary to console"""
    total_tests = sum(r.total for r in results)
    total_passed = sum(r.passed for r in results)
    total_failed = sum(r.failed for r in results)
    total_errors = sum(r.errors for r in results)

    print("\n" + "=" * 70)
    print("MAYTERAOS COMPREHENSIVE TEST REPORT")
    print("=" * 70)
    print(f"Timestamp: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    print()

    for result in results:
        status = "PASS" if result.failed == 0 and result.errors == 0 else "FAIL"
        status_color = "\033[92m" if status == "PASS" else "\033[91m"
        reset = "\033[0m"

        print(f"{status_color}[{status}]{reset} Suite: {result.suite_name}")
        print(f"      Total: {result.total} | Passed: {result.passed} | "
              f"Failed: {result.failed} | Errors: {result.errors}")
        print(f"      Time: {result.total_time_ms:.1f}ms")
        print()

        for outcome in result.outcomes:
            result_colors = {
                "PASS": "\033[92m",
                "FAIL": "\033[91m",
                "ERROR": "\033[93m",
                "TIMEOUT": "\033[93m",
                "SKIP": "\033[94m"
            }
            color = result_colors.get(outcome.result.value, "")
            print(f"        {color}{outcome.result.value:7}\033[0m "
                  f"{outcome.test.name} ({outcome.duration_ms:.1f}ms)")
            if outcome.error:
                print(f"                Error: {outcome.error[:50]}")

        print()

    print("=" * 70)
    pass_rate = (total_passed / total_tests * 100) if total_tests > 0 else 0
    print(f"OVERALL: {total_tests} tests | {total_passed} passed | "
          f"{total_failed} failed | {total_errors} errors")
    print(f"PASS RATE: {pass_rate:.1f}%")
    print("=" * 70)

    return total_failed == 0 and total_errors == 0


def main():
    parser = argparse.ArgumentParser(
        description="MayteraOS Full Test Suite Runner"
    )
    parser.add_argument("--vm", type=int, default=1002,
                        help="VM ID to test (default: 1002)")
    parser.add_argument("--suite", type=str,
                        help="Run specific suite (basic, syscall, ipc, stress, capability)")
    parser.add_argument("--list", action="store_true",
                        help="List all available test suites")
    parser.add_argument("--output", type=str, default="test_results_full.json",
                        help="JSON output file")
    parser.add_argument("--html-report", action="store_true",
                        help="Generate HTML report")
    parser.add_argument("--no-reset", action="store_true",
                        help="Don't reset VM before testing")
    parser.add_argument("--timeout", type=float, default=15.0,
                        help="Default serial timeout")
    parser.add_argument("--skip-stress", action="store_true",
                        help="Skip stress tests (faster)")

    args = parser.parse_args()

    # Create VM controller
    vm = VMController(args.vm, serial_timeout=args.timeout)

    # Create test framework
    framework = TestFramework(vm)

    # Register all tests
    logger.info("Registering test suites...")
    create_default_tests(framework)
    register_syscall_tests(framework)
    register_ipc_tests(framework)
    register_capability_tests(framework)

    if not args.skip_stress:
        register_stress_tests(framework)

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
        logger.info("Waiting for VM to boot...")
        time.sleep(15)
    elif not args.no_reset:
        logger.info("Resetting VM for clean test...")
        vm.reset()
        time.sleep(10)

    # Connect to serial
    if not vm.connect_serial():
        logger.error("Failed to connect to serial port")
        return 1

    # Wait for boot prompt
    logger.info("Waiting for shell prompt...")
    found, output = vm.read_until("maytera>", timeout_sec=90)
    if not found:
        logger.error("VM did not reach shell prompt within timeout")
        logger.info(f"Output received: {output[:500]}")
        return 1

    logger.info("VM booted successfully, starting tests...")

    # Run tests
    results = []

    if args.suite:
        if args.suite not in framework.test_suites:
            logger.error(f"Unknown suite: {args.suite}")
            logger.info(f"Available: {framework.get_available_suites()}")
            return 1
        result = framework.run_suite(args.suite)
        results.append(result)
    else:
        # Run all suites
        for suite_name in framework.get_available_suites():
            logger.info(f"Running suite: {suite_name}")
            result = framework.run_suite(suite_name)
            results.append(result)

            # Brief pause between suites
            time.sleep(2)

    # Print summary
    success = print_summary(results)

    # Save JSON results
    output_path = Path(args.output)
    with open(output_path, 'w') as f:
        json.dump([r.to_dict() for r in results], f, indent=2)
    logger.info(f"JSON results saved to {output_path}")

    # Generate HTML report if requested
    if args.html_report:
        html_path = output_path.with_suffix('.html')
        generate_html_report(results, str(html_path))

    # Cleanup
    vm.disconnect_serial()

    return 0 if success else 1


if __name__ == "__main__":
    sys.exit(main())
