#!/usr/bin/env python3
"""构建并运行 TSan monitor 示例，产出 IR 与通道可视化结果。"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import List

REPO_ROOT = Path(__file__).resolve().parents[3]
EXAMPLE_DIR = Path(__file__).resolve().parent
PRINT_CHANNEL = REPO_ROOT / "tools/tsan-monitor/scripts/print_channel.py"

# 支持的 example
SUPPORTED_EXAMPLE = [
    # Stage-2: Basic race detection
    "rw", "ww", "struct_race", "shared_counter", "lazy_init", "array_race",
    "bank_account", "linked_list_race", "producer_consumer",

    # Stage-3: Thread lifecycle (spawn/join/exit)
    "t1_parent_write_child_read",
    "t2_child_write_parent_read",
    "t3_no_join_race",
    "t4_nested_spawn",
    "t5_nested_join_race",
    "t6_multiple_children",
    "t7_partial_join_race",
    "t8_diamond_fork_join",

    # Stage-4: Mutex synchronization
    "mutex_lock_unlock",
    "mutex_race_missing_lock",
    "mutex_two_locks",
    "mutex_wrong_lock",
    "mutex_multiple_acquire",
    "mutex_lock_order",
    "mutex_read_write_lock",
    "mutex_read_write_lock_posix",

    # Stage-4: Atomic operations
    "atomic_release_acquire",
    "atomic_relaxed_race",
    "atomic_seq_cst",
    # "atomic_acq_rel",
    "atomic_cas_success",
    "atomic_cas_fail",
    "atomic_fetch_add",
    "atomic_exchange",
    "atomic_write_write_race",
    # "atomic_double_checked_locking",
    "atomic_broken_double_check",

    # Stage-4: Mixed patterns
    "mutex_atomic_mix",
]

DEFAULT_CONFIG = {
    "example": "write",
    "clang": str(REPO_ROOT / "build/bin/clang"),
    "clangxx": str(REPO_ROOT / "build/bin/clang++"),
    "monitor": str(REPO_ROOT / "build/tsan-monitor/tsan-monitor"),
    "out": str(REPO_ROOT / "tools/tsan-monitor/example/out"),
    "cflags": "",
    "ldflags": "",
    "verbose": False,
}

VERBOSE = 0

def parse_args() -> argparse.Namespace:
  parser = argparse.ArgumentParser(description="构建并运行 TSan monitor 示例")
  parser.add_argument("--example", default=DEFAULT_CONFIG["example"], choices=SUPPORTED_EXAMPLE,
                      help="示例程序名称")
  parser.add_argument("--clang", default=DEFAULT_CONFIG["clang"], help="clang 可执行文件")
  parser.add_argument("--clangxx", default=DEFAULT_CONFIG["clangxx"], help="clang++ 可执行文件")
  parser.add_argument("--monitor", default=DEFAULT_CONFIG["monitor"], help="tsan-monitor 可执行文件")
  parser.add_argument("--out", default=DEFAULT_CONFIG["out"], help="输出目录")
  parser.add_argument("--cflags", default=DEFAULT_CONFIG["cflags"], help="额外编译参数")
  parser.add_argument("--ldflags", default=DEFAULT_CONFIG["ldflags"], help="额外链接参数")
  parser.add_argument("--verbose", default=DEFAULT_CONFIG["verbose"], help="打印调试信息")
  return parser.parse_args()


def split_flags(flags: str) -> List[str]:
  return [tok for tok in flags.split() if tok]


def build_example(args: argparse.Namespace,
                  src_path: Path,
                  example_dir: Path,
                  clang: Path,
                  clangxx: Path) -> tuple[Path, Path]:
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
  ] + thread_flags + split_flags(args.cflags)

  link_cmd = [
      str(linker),
      "-fsanitize=thread",
      "-g",
      str(src_path),
      "-o",
      str(exe_path),
  ] + thread_flags + split_flags(args.cflags) + split_flags(args.ldflags)

  print("[+] 生成 LLVM IR:", " ".join(compile_cmd))
  subprocess.run(compile_cmd, check=True)

  print("[+] 构建可执行文件:", " ".join(link_cmd))
  subprocess.run(link_cmd, check=True)

  return ir_path, exe_path


def run_with_monitor(exe: Path, monitor_path: Path, example_dir: Path) -> Path:
  env = os.environ.copy()
  env["TSAN_OPTIONS"] = f"monitor_path={monitor_path}:monitor_verbose={VERBOSE}"

  print(f"[+] 运行示例，等待 monitor 退出（环境变量 {env["TSAN_OPTIONS"]}）")
  proc = subprocess.Popen([str(exe)], env=env, cwd=example_dir)
  pid = proc.pid
  ret = proc.wait()
  if ret != 0:
    raise RuntimeError(f"示例程序退出码 {ret}")

  channel_root = Path("/tmp") / f"tsan.monitor.{pid}"
  for _ in range(100):
    if channel_root.exists():
      break
    time.sleep(0.1)
  else:
    raise FileNotFoundError(f"未找到通道目录: {channel_root}")

  time.sleep(0.5)

  copy_root = example_dir / "channels"
  if copy_root.exists():
    shutil.rmtree(copy_root)
  shutil.copytree(channel_root, copy_root)
  print(f"[+] 通道文件保存到 {copy_root}")
  return copy_root


def render_channels(channel_dir: Path, example_dir: Path) -> None:
  vis_dir = example_dir / "visualized"
  vis_dir.mkdir(exist_ok=True)
  for channel_file in sorted(channel_dir.iterdir()):
    if not channel_file.is_file():
      continue
    tid = channel_file.name
    txt_path = vis_dir / f"channel_{tid}.txt"
    cmd = [sys.executable, str(PRINT_CHANNEL), str(channel_file)]
    print(f"[+] 渲染 {channel_file} -> {txt_path}")
    with txt_path.open("w", encoding="utf-8") as fp:
      subprocess.run(cmd, check=True, stdout=fp)


def merge_with_defaults(cmd: argparse.Namespace) -> argparse.Namespace:
  merged = DEFAULT_CONFIG.copy()
  for key in ["example", "clang", "clangxx", "monitor", "out", "cflags", "ldflags", "verbose"]:
    value = getattr(cmd, key)
    if value:
      merged[key] = value
  return argparse.Namespace(**merged)


def main() -> None:
  cmd_args = parse_args()
  args = merge_with_defaults(cmd_args)

  src_path = EXAMPLE_DIR / f"{args.example}.cpp"
  if not src_path.exists():
      raise FileNotFoundError(f"example 源文件不存在: {src_path}")

  out_root = Path(args.out).resolve()
  out_root.mkdir(parents=True, exist_ok=True)
  example_dir = out_root / args.example

  # 不要使用 resolve()，clang/clang++ 可能是符号链接
  clang = Path(args.clang)
  clangxx = Path(args.clangxx)
  monitor_path = Path(args.monitor).resolve()

  if not PRINT_CHANNEL.exists():
    raise FileNotFoundError(f"print_channel.py 未找到: {PRINT_CHANNEL}")
  if not monitor_path.exists():
    raise FileNotFoundError(f"monitor 可执行文件不存在: {monitor_path}")
  if not clang.exists():
    raise FileNotFoundError(f"clang 未找到: {clang}")
  if not clangxx.exists():
    raise FileNotFoundError(f"clang++ 未找到: {clangxx}")

  global VERBOSE
  VERBOSE = 1 if int(args.verbose) else 0

  ir_path, exe_path = build_example(args, src_path, example_dir, clang, clangxx)
  channel_dir = run_with_monitor(exe_path, monitor_path, example_dir)
  render_channels(channel_dir, example_dir)

  print(f"IR 输出: {ir_path}")
  print(f"可执行文件: {exe_path}")
  print(f"通道目录: {channel_dir}")
  print(f"可视化输出: {example_dir / 'visualized'}")


if __name__ == "__main__":
  main()
