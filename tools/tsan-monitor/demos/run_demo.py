#!/usr/bin/env python3
"""构建并运行 TSan monitor demo，生成 IR 与通道可视化结果。

脚本工作流程：
  1. 使用指定 clang/clang++ 编译 demo 源码，生成 LLVM IR (.ll) 与可执行文件。
  2. 设置 `TSAN_OPTIONS=monitor_path=<monitor>` 运行可执行文件，等待 monitor 退出。
  3. 根据被测程序的 PID 收集 `/tmp/tsan.monitor.<pid>/` 下的所有通道文件，
     复制到输出目录并调用 `print_channel.py` 生成可读表格。

示例：
    ./run_demo.py \
      --demo ww_demo \
      --clang /opt/homebrew/opt/llvm/bin/clang \
      --clangxx /opt/homebrew/opt/llvm/bin/clang++ \
      --monitor ../../build/bin/tsan-monitor \
      --out ../../build/tsan-monitor-demo/ww

所有路径参数都会在脚本内部转换为绝对路径；输出目录会自动创建。
"""

from __future__ import annotations

import argparse
import os
import time
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEMOS_DIR = Path(__file__).resolve().parent
PRINT_CHANNEL = REPO_ROOT / "tools/tsan-monitor/scripts/print_channel.py"

# 目前支持的 demo
SUPPORTED_DEMOS = ["ww_demo", "rw_demo"]

# 默认配置，可在脚本顶部直接修改；命令行参数会覆盖对应项。
DEFAULT_CONFIG = {
    "demo": "rw_demo",
    "clang": "",
    "clangxx": "",
    "monitor": str(REPO_ROOT / "build/bin/tsan-monitor"),
    "out": str(REPO_ROOT / "build/tsan-monitor-demo"),
    "cflags": "",
    "ldflags": "",
}


def parse_args() -> argparse.Namespace:
  parser = argparse.ArgumentParser(description="构建并运行 TSan monitor demo")
  parser.add_argument("--demo", default=DEFAULT_CONFIG["demo"], choices=SUPPORTED_DEMOS,
                      help="demo 源码文件名")
  parser.add_argument("--clang", default=DEFAULT_CONFIG["clang"],
                      help="clang 可执行文件路径")
  parser.add_argument("--clangxx", default=DEFAULT_CONFIG["clangxx"],
                      help="clang++ 可执行文件路径")
  parser.add_argument("--monitor", default=DEFAULT_CONFIG["monitor"],
                      help="已构建的 tsan-monitor 可执行文件路径")
  parser.add_argument("--out", default=DEFAULT_CONFIG["out"],
                      help="输出目录，用于保存 IR/可执行/日志/可视化")
  parser.add_argument("--cflags", default=DEFAULT_CONFIG["cflags"], help="额外透传给 clang 的编译选项")
  parser.add_argument("--ldflags", default=DEFAULT_CONFIG["clangxx"], help="额外透传给 clang 的链接选项")
  return parser.parse_args()


def split_flags(flags: str) -> list[str]:
  return [token for token in flags.split() if token]


def build_demo(args: argparse.Namespace,
               demo_src: Path,
               out_dir: Path,
               clang: Path,
               clangxx: Path) -> tuple[Path, Path]:
  ir_path = out_dir / f"{args.demo}.ll"
  exe_path = out_dir / args.demo

  compile_cmd = [
      str(clangxx),
      "-fsanitize=thread",
      "-g",
      "-S",
      "-emit-llvm",
      str(demo_src),
      "-o",
      str(ir_path),
  ] + split_flags(args.cflags)

  link_cmd = [
      str(clangxx),
      "-fsanitize=thread",
      "-g",
      str(demo_src),
      "-o",
      str(exe_path),
  ] + split_flags(args.cflags) + split_flags(args.ldflags)

  print("[+] 生成 LLVM IR:", " ".join(compile_cmd))
  subprocess.run(compile_cmd, check=True)

  print("[+] 构建可执行文件:", " ".join(link_cmd))
  subprocess.run(link_cmd, check=True)

  return ir_path, exe_path


def run_with_monitor(exe: Path, monitor_path: Path, out_dir: Path) -> Path:
  env = os.environ.copy()
  env["TSAN_OPTIONS"] = f"monitor_path={monitor_path}"

  print("[+] 运行 demo，等待 monitor 退出")
  proc = subprocess.Popen([str(exe)], env=env, cwd=out_dir)
  pid = proc.pid
  ret = proc.wait()
  if ret != 0:
    raise RuntimeError(f"demo 退出码 {ret}, 请检查运行日志")

  channel_root = Path("/tmp") / f"tsan.monitor.{pid}"
  for _ in range(100):
    if channel_root.exists():
      break
    time.sleep(0.1)
  else:
    raise FileNotFoundError(f"未找到通道目录: {channel_root}")

  # monitor 进程可能仍在刷新文件，稍作等待。
  time.sleep(0.5)

  copy_root = out_dir / "channels"
  if copy_root.exists():
    shutil.rmtree(copy_root)
  shutil.copytree(channel_root, copy_root)
  print(f"[+] 通道文件保存到 {copy_root}")
  return copy_root


def render_channels(channel_dir: Path, out_dir: Path) -> None:
  vis_dir = out_dir / "visualized"
  vis_dir.mkdir(exist_ok=True)
  for channel_file in sorted(channel_dir.iterdir()):
    if not channel_file.is_file():
      continue
    tid = channel_file.name
    txt_path = vis_dir / f"channel_{tid}.txt"
    cmd = [sys.executable, str(PRINT_CHANNEL), str(channel_file)]
    print(f"[+] 渲染 {channel_file} -> {txt_path}")
    with txt_path.open("w", encoding="utf-8") as out_fp:
      subprocess.run(cmd, check=True, stdout=out_fp)


def merge_with_defaults(cmd: argparse.Namespace) -> argparse.Namespace:
  merged = DEFAULT_CONFIG.copy()
  if cmd.demo:
    merged["demo"] = cmd.demo
  if cmd.clang:
    merged["clang"] = cmd.clang
  if cmd.clangxx:
    merged["clangxx"] = cmd.clangxx
  if cmd.monitor:
    merged["monitor"] = cmd.monitor
  if cmd.out:
    merged["out"] = cmd.out
  if cmd.cflags:
    merged["cflags"] = cmd.cflags
  if cmd.ldflags:
    merged["ldflags"] = cmd.ldflags
  return argparse.Namespace(**merged)


def main() -> None:
  cmd_args = parse_args()
  args = merge_with_defaults(cmd_args)

  demo_src = DEMOS_DIR / f"{args.demo}.cc"
  if not demo_src.exists():
    raise FileNotFoundError(f"demo 源文件不存在: {demo_src}")

  out_dir = Path(args.out).resolve()
  out_dir.mkdir(parents=True, exist_ok=True)

  clang = Path(args.clang).resolve()
  clangxx = Path(args.clangxx).resolve()
  monitor_path = Path(args.monitor).resolve()

  if not PRINT_CHANNEL.exists():
    raise FileNotFoundError(f"print_channel.py 未找到: {PRINT_CHANNEL}")
  if not monitor_path.exists():
    raise FileNotFoundError(f"monitor 可执行文件不存在: {monitor_path}")
  if not clang.exists():
    raise FileNotFoundError(f"clang 未找到: {clang}")
  if not clangxx.exists():
    raise FileNotFoundError(f"clang++ 未找到: {clangxx}")

  ir_path, exe_path = build_demo(args, demo_src, out_dir, clang, clangxx)
  channel_dir = run_with_monitor(exe_path, monitor_path, out_dir)
  render_channels(channel_dir, out_dir)

  print(f"IR 输出: {ir_path}")
  print(f"可执行文件: {exe_path}")
  print(f"通道目录: {channel_dir}")
  print(f"可视化输出: {out_dir / 'visualized'}")


if __name__ == "__main__":
  main()
