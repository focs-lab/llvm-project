"""Main test file for TSan monitor example programs."""

import pytest

from utils.example_builder import ExampleBuilder
from utils.example_runner import ExampleRunner
from utils.result_validator import ResultValidator
from utils.failure_handler import FailureHandler


def load_test_cases(category: str):
    """Load test cases from configuration by category.

    Args:
        category: Category of test cases to load

    Returns:
        List of test case dictionaries
    """
    from conftest import load_test_config
    config = load_test_config()
    return config["test_cases"].get(category, [])


def _parametrize_cases(category: str):
    """Create pytest parameters for a given category with readable IDs."""
    mark = getattr(pytest.mark, category)
    params = []
    for case in load_test_cases(category):
        params.append(pytest.param(case, id=case["name"], marks=mark))
    return params


class TestExamples:
    """Test suite for TSan monitor example programs."""

    @pytest.mark.parametrize("test_case", _parametrize_cases("should_have_race"))
    def test_should_detect_race(self, test_case, example_builder,
                              example_runner, result_validator,
                              temp_output_dir, failure_handler, example_dir):
        """Test examples that should detect data races.

        Args:
            test_case: Test case configuration
            example_builder: Fixture for building examples
            example_runner: Fixture for running examples
            result_validator: Fixture for validating results
            temp_output_dir: Temporary directory for test artifacts
            failure_handler: Fixture for handling test failures
            example_dir: Directory containing example sources
        """
        run_result = None
        try:
            # Build the example program
            source_file = example_dir / test_case["source"]
            exe_path = example_builder.build_example(source_file, temp_output_dir)

            # Also generate IR for debugging
            example_builder.build_ir(source_file, temp_output_dir)

            # Run the example program
            run_result = example_runner.run_example(exe_path, temp_output_dir)

            # Validate the result
            validation = result_validator.validate_result(run_result)

            # Check expectations - Monitor stops at first race, so we just check for presence
            actual_races = result_validator.get_race_count(validation)

            # Assertions - just check if any race was detected
            assert validation.has_race, (
                f"Expected race detection but found none in {test_case['name']}"
            )
            # Since Monitor stops at first race, any positive number of races is fine
            assert actual_races > 0, (
                f"Expected race detection but found no race files in {test_case['name']}"
            )

        except AssertionError as e:
            # Test failed - save artifacts and generate reports
            failure_handler.handle_test_failure(
                test_case["name"],
                temp_output_dir,
                run_result,
                str(e)
            )
            # Re-raise to let pytest mark the test as failed
            raise

    @pytest.mark.parametrize("test_case", _parametrize_cases("should_not_have_race"))
    def test_should_not_detect_race(self, test_case, example_builder,
                                  example_runner, result_validator,
                                  temp_output_dir, failure_handler, example_dir):
        """Test examples that should NOT detect data races.

        Args:
            test_case: Test case configuration
            example_builder: Fixture for building examples
            example_runner: Fixture for running examples
            result_validator: Fixture for validating results
            temp_output_dir: Temporary directory for test artifacts
            failure_handler: Fixture for handling test failures
            example_dir: Directory containing example sources
        """
        run_result = None
        try:
            # Build the example program
            source_file = example_dir / test_case["source"]
            exe_path = example_builder.build_example(source_file, temp_output_dir)

            # Also generate IR for debugging
            example_builder.build_ir(source_file, temp_output_dir)

            # Run the example program
            run_result = example_runner.run_example(exe_path, temp_output_dir)

            # Validate the result
            validation = result_validator.validate_result(run_result)

            # Check for unexpected races
            actual_races = result_validator.get_race_count(validation)

            # Assertions
            assert not validation.has_race, (
                f"Expected no races, but found {actual_races} in {test_case['name']}"
            )
            assert actual_races == 0, (
                f"Expected no races, but found {actual_races} in {test_case['name']}"
            )

        except AssertionError as e:
            # Test failed - save artifacts and generate reports
            failure_handler.handle_test_failure(
                test_case["name"],
                temp_output_dir,
                run_result,
                str(e)
            )
            # Re-raise to let pytest mark the test as failed
            raise

    @pytest.mark.parametrize("test_case", _parametrize_cases("maybe_have_race"))
    def test_maybe_have_race(self, test_case, example_builder,
                           example_runner, result_validator,
                           temp_output_dir, example_dir):
        """Test examples that may or may not detect races (edge cases).

        These tests are informational only - they collect results but don't
        make assertions about whether races should or shouldn't be detected.

        Args:
            test_case: Test case configuration
            example_builder: Fixture for building examples
            example_runner: Fixture for running examples
            result_validator: Fixture for validating results
            temp_output_dir: Temporary directory for test artifacts
            example_dir: Directory containing example sources
        """
        # Build the example program
        source_file = example_dir / test_case["source"]
        exe_path = example_builder.build_example(source_file, temp_output_dir)

        # Also generate IR for debugging
        example_builder.build_ir(source_file, temp_output_dir)

        # Run the example program
        run_result = example_runner.run_example(exe_path, temp_output_dir)

        # Validate the result
        validation = result_validator.validate_result(run_result)

        # Collect information without making assertions
        actual_races = result_validator.get_race_count(validation)
        has_channels = result_validator.has_channel_files(validation)

        # Print information for informational purposes
        print(f"\n[INFO] {test_case['name']}:")
        print(f"  Description: {test_case['description']}")
        print(f"  Race detected: {'Yes' if validation.has_race else 'No'}")
        print(f"  Race files: {actual_races}")
        print(f"  Channel files: {'Yes' if has_channels else 'No'}")
        print(f"  Exit code: {run_result.exit_code}")

        # Always pass - these are informational tests
        assert True



# Integration test to run all examples and collect statistics
@pytest.mark.integration
def test_all_examples_summary(example_builder, example_runner, result_validator,
                            tmp_path, example_dir):
    """Run all examples and provide a summary of results.

    This is an integration test that runs all examples and provides
    a statistical summary of race detection results.

    Args:
        example_builder: Fixture for building examples
        example_runner: Fixture for running examples
        result_validator: Fixture for validating results
        tmp_path: Temporary directory
        example_dir: Directory containing example sources
    """
    # Load all test cases
    all_test_cases = []
    all_test_cases.extend(load_test_cases("should_have_race"))
    all_test_cases.extend(load_test_cases("should_not_have_race"))
    all_test_cases.extend(load_test_cases("maybe_have_race"))

    # Statistics tracking
    stats = {
        "total_run": 0,
        "build_failed": 0,
        "run_failed": 0,
        "races_detected": 0,
        "races_expected": 0,
        "unexpected_races": 0,
        "successful_runs": 0
    }

    # Create summary directory
    summary_dir = tmp_path / "summary"
    summary_dir.mkdir()

    # Run each test case
    for test_case in all_test_cases:
        stats["total_run"] += 1

        try:
            # Create temporary directory for this test
            test_dir = summary_dir / f"test_{test_case['name']}"
            test_dir.mkdir()

            # Build the example
            source_file = example_dir / test_case["source"]
            exe_path = example_builder.build_example(source_file, test_dir)
            example_builder.build_ir(source_file, test_dir)

            # Run the example
            run_result = example_runner.run_example(exe_path, test_dir)
            validation = result_validator.validate_result(run_result)

            # Update statistics
            if validation.has_race:
                stats["races_detected"] += 1
                race_count = result_validator.get_race_count(validation)
                print(f"[SUMMARY] {test_case['name']}: {race_count} races detected")

            # Check expectations
            if test_case in load_test_cases("should_have_race"):
                stats["races_expected"] += 1
                if not validation.has_race:
                    stats["unexpected_races"] += 1
                    print(f"[SUMMARY] {test_case['name']}: UNEXPECTED - no race detected")
                else:
                    race_count = result_validator.get_race_count(validation)
                    print(f"[SUMMARY] {test_case['name']}: Race detected ({race_count} file(s))")
            elif test_case in load_test_cases("should_not_have_race"):
                if validation.has_race:
                    stats["unexpected_races"] += 1
                    race_count = result_validator.get_race_count(validation)
                    print(f"[SUMMARY] {test_case['name']}: UNEXPECTED - race detected ({race_count} file(s))")
                else:
                    print(f"[SUMMARY] {test_case['name']}: No race detected (expected)")

            stats["successful_runs"] += 1

        except Exception as e:
            stats["run_failed"] += 1
            print(f"[SUMMARY] {test_case['name']}: FAILED - {e}")

    # Print summary
    print(f"\n{'='*60}")
    print("TSAN MONITOR TEST SUMMARY")
    print(f"{'='*60}")
    print(f"Total examples: {stats['total_run']}")
    print(f"Successful runs: {stats['successful_runs']}")
    print(f"Build failures: {stats['build_failed']}")
    print(f"Run failures: {stats['run_failed']}")
    print(f"Races detected: {stats['races_detected']}")
    print(f"Races expected: {stats['races_expected']}")
    print(f"Unexpected results: {stats['unexpected_races']}")
    print(f"{'='*60}")

    # Save summary to file
    summary_content = f"""TSan Monitor Test Summary
Generated: {__import__('datetime').datetime.now(__import__('zoneinfo').ZoneInfo('Asia/Shanghai')).strftime('%Y-%m-%d %H:%M:%S %Z')}

Statistics:
- Total examples: {stats['total_run']}
- Successful runs: {stats['successful_runs']}
- Build failures: {stats['build_failed']}
- Run failures: {stats['run_failed']}
- Races detected: {stats['races_detected']}
- Races expected: {stats['races_expected']}
- Unexpected results: {stats['unexpected_races']}

Success rate: {stats['successful_runs']}/{stats['total_run']} ({stats['successful_runs']/stats['total_run']*100:.1f}%)
Note: Monitor stops at first race detection, so each example can generate at most 1 race report
"""

    with open(summary_dir / "summary.txt", "w") as f:
        f.write(summary_content)

    # Assert that we had reasonable success
    assert stats["successful_runs"] > 0, "No tests ran successfully"
    assert stats["unexpected_races"] < stats["total_run"] * 0.3, "Too many unexpected results"
