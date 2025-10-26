"""Run example programs with TSan monitor and collect results."""

import os
import shutil
import subprocess
import time
from pathlib import Path
from typing import Optional
from dataclasses import dataclass


@dataclass
class RunResult:
    """Result of running an example program."""
    exit_code: int
    program_output: str
    channel_dir: Optional[Path]
    report_dir: Path
    log_file: Path


class ExampleRunner:
    """Runner for TSan-instrumented example programs."""

    def __init__(self, monitor_path: str):
        """Initialize runner with path to monitor binary."""
        self.monitor_path = monitor_path

    def run_example(self, exe_path: Path, output_dir: Path) -> RunResult:
        """Run an example program with monitor and collect results.

        Args:
            exe_path: Path to the executable to run
            output_dir: Directory where results should be stored

        Returns:
            RunResult containing program output and artifact locations
        """
        # Set up environment variables for TSan
        env = os.environ.copy()
        env["TSAN_OPTIONS"] = f"monitor_path={self.monitor_path}:exit_on_race=1:atexit_sleep_ms=200:monitor_verbose=1"

        # Run the program
        proc = subprocess.Popen(
            [str(exe_path.absolute())],  # Use absolute path since we're setting cwd
            env=env,
            cwd=output_dir,  # Set working directory to ensure reports are generated in test-specific directory
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True
        )

        pid = proc.pid
        output, _ = proc.communicate()

        # Wait for channel files to appear
        channel_dir = self._wait_for_channel_files(pid)
        final_channel_dir = output_dir / "channel"

        # Copy channel files to output directory if they exist
        if channel_dir and channel_dir.exists():
            final_channel_dir.mkdir(exist_ok=True)
            for channel_file in channel_dir.iterdir():
                if channel_file.is_file():
                    shutil.copy2(channel_file, final_channel_dir)

        # Set up paths for report and log files
        report_dir = output_dir / "report"
        log_file = output_dir / "tsan-monitor.log"

        return RunResult(
            exit_code=proc.returncode,
            program_output=output,
            channel_dir=final_channel_dir if final_channel_dir.exists() else None,
            report_dir=report_dir,
            log_file=log_file
        )

    def _wait_for_channel_files(self, pid: int) -> Optional[Path]:
        """Wait for channel files to appear after program execution.

        Args:
            pid: Process ID of the executed program

        Returns:
            Path to channel directory if found, None otherwise
        """
        channel_root = Path("/tmp") / f"tsan.monitor.{pid}"

        # Wait for up to 10 seconds for channel files to appear
        for attempt in range(100):  # 100 * 0.1s = 10s
            if channel_root.exists():
                # Give extra time for files to be written
                time.sleep(0.5)
                return channel_root
            time.sleep(0.1)

        return None