"""Handle test failures by saving artifacts and generating analysis reports."""

import shutil
import subprocess
from datetime import datetime
from pathlib import Path
from zoneinfo import ZoneInfo
from typing import Optional
from dataclasses import dataclass

# Import from local modules
from utils.example_runner import RunResult


@dataclass
class TestFailure:
    """Information about a test failure."""
    test_name: str
    assertion_error: Optional[str]
    timestamp: str
    save_dir: Path


class FailureHandler:
    """Handler for test failures, responsible for saving artifacts and generating reports."""

    def __init__(self, failure_save_dir: Path, print_channel_script: Path):
        """Initialize failure handler.

        Args:
            failure_save_dir: Directory where failure artifacts should be saved
            print_channel_script: Path to print_channel.py script
        """
        self.failure_save_dir = failure_save_dir
        self.print_channel_script = print_channel_script

    def handle_test_failure(self, test_name: str, temp_dir: Path,
                           run_result: Optional[RunResult], assertion_error: Optional[str] = None) -> TestFailure:
        """Handle a test failure by saving artifacts and generating analysis reports.

        Args:
            test_name: Name of the failing test
            temp_dir: Temporary directory containing test artifacts
            run_result: Result from running the example (may be None if build failed)
            assertion_error: Assertion error message (may be None)

        Returns:
            TestFailure with information about the failure
        """
        # Create failure case save directory
        timestamp = datetime.now(ZoneInfo("Asia/Shanghai")).strftime("%Y%m%d_%H%M%S")
        save_dir = self.failure_save_dir / f"{test_name}_{timestamp}"
        save_dir.mkdir(parents=True, exist_ok=True)

        # Save test information
        self._save_test_info(save_dir, test_name, assertion_error)

        # Copy all artifacts
        self._copy_artifacts(temp_dir, save_dir)

        # Generate channel analysis reports
        if run_result and run_result.channel_dir:
            self._generate_channel_reports(save_dir / "channel")

        # Generate detailed failure report
        self._generate_failure_report(save_dir, test_name, run_result, assertion_error)

        print(f"\n[FAILURE] Test '{test_name}' failed. Artifacts saved to: {save_dir}")

        return TestFailure(
            test_name=test_name,
            assertion_error=assertion_error,
            timestamp=timestamp,
            save_dir=save_dir
        )

    def _save_test_info(self, save_dir: Path, test_name: str, assertion_error: Optional[str]):
        """Save basic test information.

        Args:
            save_dir: Directory to save the information
            test_name: Name of the failing test
            assertion_error: Assertion error message
        """
        info_content = f"""Test Failure Information
========================
Test Name: {test_name}
Failure Time: {datetime.now(ZoneInfo('Asia/Shanghai')).strftime('%Y-%m-%d %H:%M:%S %Z')}
Assertion Error: {assertion_error or 'N/A'}

Expected vs Actual Results:
{self._analyze_expectation_vs_actual(test_name)}
"""

        with open(save_dir / "test_info.txt", "w") as f:
            f.write(info_content)

    def _copy_artifacts(self, temp_dir: Path, save_dir: Path):
        """Copy all test-generated artifacts to the save directory.

        Args:
            temp_dir: Temporary directory containing artifacts
            save_dir: Directory to save artifacts to
        """
        if not temp_dir.exists():
            return

        # Define patterns of files to copy
        artifact_patterns = [
            "channel",     # Channel files directory
            "report",      # Race reports directory
            "tsan-monitor.log",  # Monitor log file
            "*.exe",       # Executable files
            "*.ll",        # LLVM IR files
            "*.*"          # All other files
        ]

        for pattern in artifact_patterns:
            for item in temp_dir.glob(pattern):
                try:
                    if item.is_file():
                        shutil.copy2(item, save_dir / item.name)
                    elif item.is_dir() and not (save_dir / item.name).exists():
                        shutil.copytree(item, save_dir / item.name, dirs_exist_ok=True)
                except (OSError, shutil.Error) as e:
                    print(f"[WARNING] Failed to copy {item}: {e}")

    def _generate_channel_reports(self, channel_dir: Path):
        """Generate channel analysis reports using print_channel.py.

        Args:
            channel_dir: Directory containing channel files
        """
        if not channel_dir.exists():
            return

        print(f"[DEBUG] Generating channel reports for {channel_dir}")

        for channel_file in channel_dir.iterdir():
            if not channel_file.is_file():
                continue

            try:
                cmd = [
                    "python3",
                    str(self.print_channel_script),
                    str(channel_file),
                    "--format", "both",
                    "--output-dir", str(channel_dir)
                ]

                result = subprocess.run(
                    cmd,
                    capture_output=True,
                    text=True,
                    check=True
                )
                print(f"[DEBUG] Generated reports for {channel_file.name}")

                # Also capture print_channel.py output for debugging
                if result.stdout:
                    with open(channel_dir / "print_channel_output.txt", "a") as f:
                        f.write(f"Output for {channel_file.name}:\n{result.stdout}\n")

            except subprocess.CalledProcessError as e:
                print(f"[ERROR] Failed to generate reports for {channel_file}: {e}")
                if e.stderr:
                    print(f"[ERROR] stderr: {e.stderr}")

    def _generate_failure_report(self, save_dir: Path, test_name: str,
                                run_result: Optional[RunResult], assertion_error: Optional[str]):
        """Generate a detailed failure report.

        Args:
            save_dir: Directory to save the report
            test_name: Name of the failing test
            run_result: Result from running the example
            assertion_error: Assertion error message
        """
        program_output = run_result.program_output if run_result else "No program output (build failed)"
        exit_code = run_result.exit_code if run_result else -1

        race_count = 0
        if run_result and run_result.report_dir.exists():
            race_count = len([f for f in run_result.report_dir.glob("*.txt")])

        channel_count = 0
        if run_result and run_result.channel_dir and run_result.channel_dir.exists():
            channel_count = len([f for f in run_result.channel_dir.iterdir()])

        report_content = f"""Test Failure Report
==================

Test Name: {test_name}
Timestamp: {datetime.now(ZoneInfo('Asia/Shanghai')).strftime('%Y-%m-%d %H:%M:%S %Z')}

Assertion Error:
{assertion_error or 'No specific assertion error'}

Program Execution Results:
-------------------------
Exit Code: {exit_code}
Program Output:
{program_output}

Race Detection Results:
---------------------
Report Directory: {'EXISTS' if run_result and run_result.report_dir.exists() else 'MISSING'}
Race Files Found: {race_count}

Channel Analysis:
----------------
Channel Directory: {'EXISTS' if run_result and run_result.channel_dir and run_result.channel_dir.exists() else 'MISSING'}
Channel Files Found: {channel_count}

Generated Reports:
-----------------
- test_info.txt: Basic test information
- channel/*.slot: Slot-based channel analysis (if channel files exist)
- channel/*.event: Event-based channel analysis (if channel files exist)
- report/: Original race reports (if any races detected)
- tsan-monitor.log: Monitor execution log (if available)
- print_channel_output.txt: Output from print_channel.py script

Debugging Information:
--------------------
Monitor Path: {run_result.report_dir.parent.parent.parent if run_result else 'Unknown'}
Timestamp: {datetime.now(ZoneInfo('Asia/Shanghai')).strftime('%Y-%m-%d %H:%M:%S %Z')}
"""

        with open(save_dir / "failure_report.txt", "w") as f:
            f.write(report_content)

    def _analyze_expectation_vs_actual(self, test_name: str) -> str:
        """Analyze the difference between expected and actual results.

        Args:
            test_name: Name of the failing test

        Returns:
            String describing the expectation vs actual result
        """
        test_name_lower = test_name.lower()

        if any(keyword in test_name_lower for keyword in ["race", "ww", "rw", "struct"]):
            return "Expected: Race detected\nActual: No race found or insufficient races detected"
        elif any(keyword in test_name_lower for keyword in ["mutex", "atomic", "lock"]):
            return "Expected: No race detected\nActual: Race found unexpectedly"
        elif "t" in test_name_lower and test_name_lower.startswith("t"):
            return "Expected: Synchronization behavior\nActual: Unexpected race or missing synchronization"
        else:
            return "Expected: Known behavior\nActual: Unexpected result"

    def cleanup_old_failures(self, max_failures: int = 10):
        """Clean up old failure directories to prevent disk space issues.

        Args:
            max_failures: Maximum number of failure directories to keep
        """
        if not self.failure_save_dir.exists():
            return

        # Get all failure directories sorted by modification time
        failure_dirs = [
            d for d in self.failure_save_dir.iterdir()
            if d.is_dir() and any(char.isdigit() for char in d.name)
        ]
        failure_dirs.sort(key=lambda d: d.stat().st_mtime, reverse=True)

        # Remove old directories beyond the limit
        for old_dir in failure_dirs[max_failures:]:
            try:
                shutil.rmtree(old_dir)
                print(f"[CLEANUP] Removed old failure directory: {old_dir}")
            except OSError as e:
                print(f"[WARNING] Failed to remove {old_dir}: {e}")