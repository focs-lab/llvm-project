"""Pytest configuration and fixtures for TSan monitor tests."""

import pytest
import yaml
import shutil
from pathlib import Path


def load_test_config():
    """Load test configuration from YAML file."""
    config_path = Path(__file__).parent / "test_cases.yaml"
    with open(config_path, "r") as f:
        return yaml.safe_load(f)


@pytest.fixture(scope="session")
def config():
    """Load and provide test configuration."""
    return load_test_config()


@pytest.fixture(scope="session")
def example_builder(config):
    """Create and provide an ExampleBuilder instance."""
    from utils.example_builder import ExampleBuilder

    build_config = config["build_config"]
    return ExampleBuilder(
        build_config["clang"],
        build_config["monitor_path"]
    )


@pytest.fixture(scope="session")
def example_runner(config):
    """Create and provide an ExampleRunner instance."""
    from utils.example_runner import ExampleRunner

    return ExampleRunner(config["build_config"]["monitor_path"])


@pytest.fixture
def result_validator():
    """Create and provide a ResultValidator instance."""
    from utils.result_validator import ResultValidator
    return ResultValidator()


@pytest.fixture
def failure_handler(config):
    """Create and provide a FailureHandler instance."""
    from utils.failure_handler import FailureHandler

    failure_save_dir = Path(config["output_config"]["failure_save_dir"])
    print_channel_script = Path(config["output_config"]["print_channel_script"])

    # Ensure failure save directory exists
    failure_save_dir.mkdir(exist_ok=True)

    handler = FailureHandler(failure_save_dir, print_channel_script)

    # Clean up old failures before running tests
    handler.cleanup_old_failures()

    return handler


@pytest.fixture
def temp_output_dir(tmp_path, config):
    """Create and provide a temporary output directory for tests."""
    temp_base = Path(config["output_config"]["temp_base_dir"])
    temp_base.mkdir(exist_ok=True)

    # Create unique temporary directory
    import uuid
    unique_id = str(uuid.uuid4())[:8]  # Use short UUID to avoid long paths
    output_dir = temp_base / f"test_{unique_id}"
    output_dir.mkdir()

    yield output_dir

    # Cleanup: remove temporary directory after test
    if output_dir.exists():
        shutil.rmtree(output_dir)


@pytest.fixture
def example_dir():
    """Provide path to the examples directory."""
    return Path(__file__).parent.parent / "example"


def load_test_cases(category: str):
    """Load test cases from configuration by category.

    Args:
        category: Category of test cases ('should_have_race', 'should_not_have_race', 'maybe_have_race')

    Returns:
        List of test case dictionaries
    """
    config = load_test_config()
    return config["test_cases"].get(category, [])


@pytest.hookimpl(tryfirst=True, hookwrapper=True)
def pytest_runtest_makereport(item, call):
    """Pytest hook to handle test reporting."""
    outcome = yield
    rep = outcome.get_result()

    # Add custom information to test report if needed
    if rep.when == "call" and rep.failed:
        # This could be used to add custom failure information
        pass


def pytest_configure(config):
    """Configure pytest with custom markers."""
    config.addinivalue_line(
        "markers",
        "slow: marks tests as slow (deselect with '-m \"not slow\"')"
    )
    config.addinivalue_line(
        "markers",
        "integration: marks tests as integration tests"
    )
    for marker, description in (
        ("should_have_race", "examples expected to contain data races"),
        ("should_not_have_race", "examples expected to be race free"),
        ("maybe_have_race", "examples with outcome-dependent race detection"),
    ):
        config.addinivalue_line(
            "markers",
            f"{marker}: {description}"
        )
