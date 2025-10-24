# TSan Monitor Test Framework

This directory contains a comprehensive test framework for TSan monitor example programs.

## Purpose

The test framework validates that the TSan monitor correctly:
- Detects data races in programs that should have them
- Does not detect false positives in programs that should not have races
- Handles edge cases and synchronization patterns

## Test Categories

### 1. Should Have Race (`should_have_race`)
Examples that are expected to detect data races:
- `ww.cpp` - Write-write race
- `rw.cpp` - Read-write race
- `struct_race.cpp` - Race on struct members
- `atomic_relaxed_race.cpp` - Relaxed atomic operations
- `mutex_race_missing_lock.cpp` - Missing mutex locks

**Note**: Monitor stops at first race detection, so each example generates at most one race report file. Tests only check if any race is detected, not the count.

### 2. Should Not Have Race (`should_not_have_race`)
Examples that should NOT detect data races:
- `mutex_lock_unlock.cpp` - Proper mutex usage
- `atomic_release_acquire.cpp` - Proper atomic acquire-release
- `atomic_seq_cst.cpp` - Sequentially consistent atomics
- `mutex_two_locks.cpp` - Independent locks on different data

### 3. Maybe Have Race (`maybe_have_race`)
Edge cases where race detection may vary:
- Thread spawn/join synchronization patterns
- Complex atomic operations
- Mixed synchronization primitives

## Usage

### Prerequisites
- Python 3.9+ with zoneinfo support
- pytest: `pip install pytest pyyaml`
- Built LLVM/TSan with monitor binary
- Built example programs in `../example/`

### Running Tests

```bash
# Run all tests
pytest -v

# Run specific categories via custom markers
pytest -v -m should_have_race       # Must detect races
pytest -v -m should_not_have_race   # Must not detect races
pytest -v -m maybe_have_race        # Edge cases (informational)

# Run specific test
pytest -v -k "write_write_race"

# Run with detailed output
pytest -v -s

# Run integration test (summary of all examples)
pytest -v -m "integration"
```

### Output

#### Successful Tests
- Temporary files are automatically cleaned up
- No artifacts are preserved

#### Failed Tests
- All artifacts are preserved in `failures/` directory
- Structure: `failures/{test_name}_{timestamp}/`
- Contents:
  - `test_info.txt` - Basic failure information
  - `failure_report.txt` - Detailed failure analysis
  - `channel/` - Channel files with analysis reports
  - `report/` - Race reports (if any)
  - `tsan-monitor.log` - Monitor execution log
  - `*.exe`, `*.ll` - Executable and IR files

#### Channel Analysis
Failed tests automatically generate:
- `channel/*.slot` - Slot-based channel analysis
- `channel/*.event` - Event-based channel analysis
- `channel/print_channel_output.txt` - Script output

## Configuration

### Test Configuration (`test_cases.yaml`)
- Define test cases and their expectations
- Configure build and output paths
- Set expected minimum race counts

### Build Configuration
- `clang`: Path to clang++ binary
- `cflags`: Compilation flags for TSan
- `monitor_path`: Path to TSan monitor binary

### Output Configuration
- `temp_base_dir`: Temporary file location
- `failure_save_dir`: Failed test artifacts location
- `print_channel_script`: Path to analysis script

## Architecture

### Components
- `ExampleBuilder`: Compiles example programs
- `ExampleRunner`: Executes programs with monitor
- `ResultValidator`: Validates race detection results
- `FailureHandler`: Handles test failures and artifact preservation

### Test Flow
1. Build example program with TSan instrumentation
2. Execute program with TSan monitor
3. Wait for channel files and race reports
4. Validate results against expectations
5. On failure: preserve artifacts and generate analysis
6. On success: clean up temporary files

## Debugging Failed Tests

1. Navigate to failure directory:
   ```bash
   cd failures/{test_name}_{timestamp}/
   ```

2. Review failure information:
   ```bash
   cat test_info.txt
   cat failure_report.txt
   ```

3. Examine channel analysis:
   ```bash
   ls channel/
   cat channel/channel_0.event
   ```

4. Check race reports:
   ```bash
   ls report/
   cat report/*.txt
   ```

5. Review monitor log:
   ```bash
   cat tsan-monitor.log
   ```

## Adding New Tests

1. Add test case to `test_cases.yaml`
2. Place source file in `../example/`
3. Choose appropriate category based on expected behavior
4. Set `expected_min_races` if needed
5. Run tests to validate

## Integration with CI

The framework is designed to integrate with CI/CD systems:
- Exit codes indicate test success/failure
- Detailed reports generated in standard pytest format
- Artifacts preserved for failed tests
- Summary statistics available via integration test

## Troubleshooting

### Common Issues

**Build Failures:**
- Check clang++ path in `test_cases.yaml`
- Verify TSan monitor binary exists
- Check source file paths

**Runtime Failures:**
- Verify monitor binary is executable
- Check for missing libraries
- Review `tsan-monitor.log` in failure directory

**No Channel Files:**
- Check monitor binary path
- Verify TSAN_OPTIONS environment variable
- Review program output for monitor errors

**Unexpected Race Results:**
- Examine channel analysis reports
- Review race report details
- Check synchronization logic in source code

### Performance

- Tests run in parallel by default
- Use `pytest -n auto` for explicit parallelization
- Temporary files cleaned up automatically
- Old failure directories pruned automatically
