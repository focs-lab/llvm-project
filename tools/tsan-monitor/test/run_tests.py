#!/usr/bin/env python3
"""Test runner script for TSan monitor tests."""

import subprocess
import sys
from pathlib import Path


def run_command(cmd, description):
    """Run a command and handle the result."""
    print(f"\n{'='*60}")
    print(f"Running: {description}")
    print(f"Command: {' '.join(cmd)}")
    print(f"{'='*60}")

    try:
        result = subprocess.run(cmd, check=True, text=True, capture_output=True)
        print(result.stdout)
        if result.stderr:
            print("STDERR:", result.stderr)
        return True
    except subprocess.CalledProcessError as e:
        print(f"Command failed with exit code {e.returncode}")
        print("STDOUT:", e.stdout)
        print("STDERR:", e.stderr)
        return False


def main():
    """Main test runner function."""
    test_dir = Path(__file__).parent

    print("TSan Monitor Test Runner")
    print(f"Test directory: {test_dir}")

    # Check if requirements are installed
    print("\n[1] Checking requirements...")
    if not run_command([
        sys.executable, "-m", "pip", "list"
    ], "Checking installed packages"):
        print("Installing requirements...")
        run_command([
            sys.executable, "-m", "pip", "install", "-r", "requirements.txt"
        ], "Installing requirements")

    # Check configuration
    print("\n[2] Checking configuration...")
    config_file = test_dir / "test_cases.yaml"
    if not config_file.exists():
        print(f"ERROR: Configuration file not found: {config_file}")
        return False

    # Check monitor binary
    print("\n[3] Checking monitor binary...")
    from .conftest import load_test_config
    config = load_test_config()
    monitor_path = Path(config["build_config"]["monitor_path"])
    if not monitor_path.exists():
        print(f"ERROR: Monitor binary not found: {monitor_path}")
        print("Please build the TSan monitor first.")
        return False

    # Check examples directory
    print("\n[4] Checking examples directory...")
    example_dir = test_dir.parent / "example"
    if not example_dir.exists():
        print(f"ERROR: Examples directory not found: {example_dir}")
        return False

    # Run tests
    print("\n[5] Running tests...")

    test_commands = [
        # Quick test to check basic functionality
        ([sys.executable, "-m", "pytest", "test_examples.py::TestExamples::test_maybe_have_race", "-v"],
         "Running quick informational tests"),

        # Run tests that should detect races
        ([sys.executable, "-m", "pytest", "-m", "should_have_race", "-v"],
         "Running tests that should detect races"),

        # Run tests that should not detect races
        ([sys.executable, "-m", "pytest", "-m", "should_not_have_race", "-v"],
         "Running tests that should not detect races"),

        # Run integration summary
        ([sys.executable, "-m", "pytest", "-m", "integration", "-v"],
         "Running integration summary"),
    ]

    success_count = 0
    total_count = len(test_commands)

    for cmd, description in test_commands:
        if run_command(cmd, description):
            success_count += 1
        else:
            print(f"FAILED: {description}")

    # Summary
    print(f"\n{'='*60}")
    print(f"Test Runner Summary")
    print(f"{'='*60}")
    print(f"Successful test runs: {success_count}/{total_count}")

    if success_count == total_count:
        print("All tests completed successfully!")
        return True
    else:
        print(f"{total_count - success_count} test runs failed.")
        print("Check the output above for details.")
        return False


if __name__ == "__main__":
    success = main()
    sys.exit(0 if success else 1)
