#!/usr/bin/env python3
"""Build and run TSan monitor example, generating IR and channel visualization results."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import List

REPO_ROOT = Path(__file__).resolve().parents[3]
EXAMPLE_DIR = Path(__file__).resolve().parent
PRINT_CHANNEL = REPO_ROOT / "tools/tsan-monitor/scripts/print_channel.py"

# Supported examples
SUPPORTED_EXAMPLE = [
    "misc_write",

    # Basic races
    "dr_rw", "dr_ww", "dr_array", "dr_struct", "dr_counter", "dr_bank",
    "dr_list", "dr_prodcons", "dr_lazy",

    # Thread lifecycle
    "th_spawn_sync", "th_join_sync", "th_no_join_race", "th_nested_spawn",
    "th_nested_join_race", "th_multi_children", "th_partial_join_race",
    "th_diamond_fork_join",

    # Mutex
    "mx_basic", "dr_mx_missing", "mx_two", "dr_mx_wrong", "mx_multi_acq",
    "mx_order", "mx_rwlock", "mx_atomic_mix",

    # Atomics
    "at_rel_acq", "at_relaxed_race", "at_seqcst", "at_acq_rel",
    "at_cas_ok", "at_cas_fail", "at_fetch_add", "at_xchg", "at_ww",
    "at_dcl_ok", "at_dcl_broken",
]

DEFAULT_CONFIG = {
    "example": "misc_write",
    "clang": str(REPO_ROOT / "build/bin/clang"),
    "clangxx": str(REPO_ROOT / "build/bin/clang++"),
    "monitor": str(REPO_ROOT / "build/tsan-monitor/tsan-monitor"),
    "out": str(REPO_ROOT / "tools/tsan-monitor/example/out"),
    "verbose": False,
}

VERBOSE = 0


def dprint(*args, **kwargs) -> None:
    """Debug print function that only outputs when verbose mode is enabled."""
    if VERBOSE:
        print(*args, **kwargs)


def parse_args() -> argparse.Namespace:
    """Parse command line arguments."""
    parser = argparse.ArgumentParser(description="Build and run TSan monitor example")
    parser.add_argument("-e", "--example", default=DEFAULT_CONFIG["example"], choices=SUPPORTED_EXAMPLE,
                      help="Example program name")
    parser.add_argument("--clang", default=DEFAULT_CONFIG["clang"], help="clang executable")
    parser.add_argument("--clangxx", default=DEFAULT_CONFIG["clangxx"], help="clang++ executable")
    parser.add_argument("-m", "--monitor", default=DEFAULT_CONFIG["monitor"], help="tsan-monitor executable")
    parser.add_argument("-o", "--out", default=DEFAULT_CONFIG["out"], help="Output directory")
    parser.add_argument("-v", "--verbose", default=DEFAULT_CONFIG["verbose"], action="store_true",
                      help="Print debug information")
    return parser.parse_args()


def build_example(args: argparse.Namespace,
                  src_path: Path,
                  example_dir: Path,
                  clang: Path,
                  clangxx: Path) -> tuple[Path, Path]:
    """Build example program with TSan instrumentation."""
    # Examples are C++ sources; always use clang++ for compile and link.
    # Keep -pthread to ensure libstdc++/libc++ threads are linked correctly.
    compiler = clangxx
    linker = clangxx
    thread_flags: List[str] = ["-pthread"]

    ir_path = example_dir / f"{args.example}.ll"
    exe_path = example_dir / args.example
    example_dir.mkdir(parents=True, exist_ok=True)

    compile_cmd = [
        str(compiler),
        "-fsanitize=thread",
        "-g",
        "-S",
        "-emit-llvm",
        str(src_path),
        "-o",
        str(ir_path),
    ] + thread_flags

    link_cmd = [
        str(linker),
        "-fsanitize=thread",
        "-g",
        str(src_path),
        "-o",
        str(exe_path),
    ] + thread_flags

    dprint(f"[+] Generating LLVM IR: {' '.join(compile_cmd)}")
    subprocess.run(compile_cmd, check=True)

    dprint(f"[+] Building executable: {' '.join(link_cmd)}")
    subprocess.run(link_cmd, check=True)

    return ir_path, exe_path


def run_with_monitor(exe: Path, monitor_path: Path, example_dir: Path) -> Path:
    """Run example with monitor and return channel directory."""
    env = os.environ.copy()
    env["TSAN_OPTIONS"] = f"monitor_path={monitor_path}:exit_on_race=1:atexit_sleep_ms=500:monitor_verbose=1"

    dprint(f"[+] Running example, waiting for monitor to exit (TSAN_OPTIONS: {env['TSAN_OPTIONS']})")

    # Execute the program and capture output
    proc = subprocess.Popen([str(exe)], env=env, cwd=example_dir,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    pid = proc.pid
    output, _ = proc.communicate()

    if ret := proc.returncode != 0:
        dprint(f"[+] Example program exited with code {ret}")

    # Print program execution result with separators
    print("=" * 80)
    print("PROGRAM EXECUTION RESULT:")
    print("-" * 80)
    print(output if output.strip() else "(No output)")
    print("=" * 80)
    print("\n")

    # Wait for channel directory to appear
    channel_root = Path("/tmp") / f"tsan.monitor.{pid}"
    for _ in range(100):
        if channel_root.exists():
            break
        time.sleep(0.1)
    else:
        raise FileNotFoundError(f"Channel directory not found: {channel_root}")

    time.sleep(0.5)
    return channel_root


def render_channels(channel_dir: Path, example_dir: Path) -> None:
    """Generate channel visualization reports using print_channel.py."""
    channel_output_dir = example_dir / "channel"
    channel_output_dir.mkdir(exist_ok=True)

    for channel_file in sorted(channel_dir.iterdir()):
        if not channel_file.is_file():
            continue

        # Use print_channel.py to generate slot or/and event formats
        cmd = [sys.executable, str(PRINT_CHANNEL), str(channel_file),
               "--format", "event", "--output-dir", str(channel_output_dir)] # ["slot", "event", "both"]

        dprint(f"[+] Processing {channel_file} with print_channel.py")
        result = subprocess.run(cmd, check=True, capture_output=True, text=True)
        dprint(result.stdout)

        if result.stderr:
            dprint(f"[!] print_channel.py stderr: {result.stderr}")


def merge_with_defaults(cmd: argparse.Namespace) -> argparse.Namespace:
    """Merge command line arguments with default configuration."""
    merged = DEFAULT_CONFIG.copy()
    for key in ["example", "clang", "clangxx", "monitor", "out", "verbose"]:
        value = getattr(cmd, key)
        if value:
            merged[key] = value
    return argparse.Namespace(**merged)


def main() -> None:
    """Main function."""
    cmd_args = parse_args()
    args = merge_with_defaults(cmd_args)

    # Set global verbose flag
    global VERBOSE
    VERBOSE = 1 if args.verbose else 0

    src_path = EXAMPLE_DIR / f"{args.example}.cpp"
    if not src_path.exists():
        raise FileNotFoundError(f"Example source file not found: {src_path}")

    out_root = Path(args.out).resolve()
    out_root.mkdir(parents=True, exist_ok=True)
    example_dir = out_root / args.example

    # Don't use resolve() for clang/clang++ as they might be symlinks
    clang = Path(args.clang)
    clangxx = Path(args.clangxx)
    monitor_path = Path(args.monitor).resolve()

    # Print essential information
    print("=" * 80)
    print("TSAN MONITOR EXAMPLE EXECUTION")
    print("=" * 80)
    print(f"Example: {args.example}")
    print(f"Clang binary: {clang}")
    print(f"Clang++ binary: {clangxx}")
    print(f"Monitor binary: {monitor_path}")
    print(f"Output directory: {example_dir}")
    print(f"Verbose mode: {'ON' if VERBOSE else 'OFF'}")
    print("=" * 80)
    print("\n")

    # Validate dependencies
    if not PRINT_CHANNEL.exists():
        raise FileNotFoundError(f"print_channel.py not found: {PRINT_CHANNEL}")
    if not monitor_path.exists():
        raise FileNotFoundError(f"Monitor executable not found: {monitor_path}")
    if not clang.exists():
        raise FileNotFoundError(f"clang not found: {clang}")
    if not clangxx.exists():
        raise FileNotFoundError(f"clang++ not found: {clangxx}")

    # Build and run
    ir_path, exe_path = build_example(args, src_path, example_dir, clang, clangxx)
    channel_dir = run_with_monitor(exe_path, monitor_path, example_dir)

    # Generate channel reports
    render_channels(channel_dir, example_dir)

    # Final summary
    dprint("=" * 80)
    dprint("EXECUTION SUMMARY:")
    dprint("=" * 80)
    dprint(f"IR output: {ir_path}")
    dprint(f"Executable: {exe_path}")
    dprint(f"Channel reports: {example_dir / 'channel'}")
    dprint("=" * 80)


if __name__ == "__main__":
    main()
