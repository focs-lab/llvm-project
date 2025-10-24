"""Validate and analyze test results from TSan monitor runs."""

from pathlib import Path
from typing import List
from dataclasses import dataclass


@dataclass
class ValidationResult:
    """Result of validating a test run."""
    has_race: bool
    race_files: List[str]
    channel_files: List[str]


class ResultValidator:
    """Validator for TSan monitor test results."""

    def validate_result(self, run_result: 'RunResult') -> ValidationResult:
        """Validate the result of running an example program.

        Args:
            run_result: Result from running the example program

        Returns:
            ValidationResult with race detection status and file lists
        """
        # Check for race reports
        has_race = self._check_race_reports(run_result.report_dir)
        race_files = self._list_race_files(run_result.report_dir)

        # List channel files
        channel_files = self._list_channel_files(run_result.channel_dir)

        return ValidationResult(
            has_race=has_race,
            race_files=race_files,
            channel_files=channel_files
        )

    def _check_race_reports(self, report_dir: Path) -> bool:
        """Check if any race reports exist.

        Args:
            report_dir: Directory containing race reports

        Returns:
            True if race reports exist, False otherwise
        """
        if not report_dir.exists():
            return False

        # Look for .txt files in report directory
        race_files = list(report_dir.glob("*.txt"))
        return len(race_files) > 0

    def _list_race_files(self, report_dir: Path) -> List[str]:
        """List all race report files.

        Args:
            report_dir: Directory containing race reports

        Returns:
            List of race report filenames
        """
        if not report_dir.exists():
            return []

        return [f.name for f in report_dir.glob("*.txt")]

    def _list_channel_files(self, channel_dir: Path) -> List[str]:
        """List all channel files.

        Args:
            channel_dir: Directory containing channel files

        Returns:
            List of channel filenames
        """
        if not channel_dir:
            return []

        if not channel_dir.exists():
            return []

        return [f.name for f in channel_dir.iterdir() if f.is_file()]

    def get_race_count(self, validation_result: ValidationResult) -> int:
        """Get the number of races detected.

        Args:
            validation_result: Result from validate_result

        Returns:
            Number of races detected
        """
        return len(validation_result.race_files)

    def has_channel_files(self, validation_result: ValidationResult) -> bool:
        """Check if any channel files exist.

        Args:
            validation_result: Result from validate_result

        Returns:
            True if channel files exist, False otherwise
        """
        return len(validation_result.channel_files) > 0