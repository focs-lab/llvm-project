#!/usr/bin/env python3
"""以人类可读的方式打印单个 TSan monitor 通道文件。

脚本会逐槽读取 64 位值，并根据当前 ABI 将事件槽与参数槽区分开来：
- 事件槽（头槽）会解析出 EventType、Lap、地址等字段；
- 参数槽以原始 64 位十六进制形式显示。

示例：
    python3 print_channel.py /tmp/tsan.monitor.<pid>/<tid>

依赖：tabulate（`pip install tabulate`）。
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path
from typing import Iterable, List

from tabulate import tabulate

# 与 Origin 端保持同步的事件类型表。
# 事件表（需与 Origin 侧保持同步）。
EVENT_TYPE_NAMES = {
    0x00: "kEventClear",
    0x01: "kEventRead",
    0x02: "kEventWrite",
    0x03: "kEventVptrUpdate",
    0x04: "kEventVptrLoad",
    0x05: "kEventMemset",
    0x06: "kEventMemcpy",
    0x07: "kEventAtomicLoad",
    0x08: "kEventAtomicStore",
    0x09: "kEventAtomicRMW",
    0x0A: "kEventAtomicCAS",
    0x0B: "kEventAtomicFence",
    0x0C: "kEventReturn",
    0x0D: "kEventAtExit",
    0x19: "kThreadSpawn",   # 25
    0x1A: "kThreadJoin",    # 26
    0x1B: "kThreadExit",    # 27
    0xFE: "kEventIgnoreBegin",
    0xFF: "kEventIgnoreEnd",
}
ALLOWED_EVENT_TYPES = set(EVENT_TYPE_NAMES.keys())

# 哨兵值：握手与程序结束。（非标准事件头，直接写入原始 u64。）
MONITOR_READY_MAGIC = 0x00000000CAFEBEEF
PROGRAM_ENDED_MAGIC = 0x00000000DEADDEAD


def parse_slot(value: int) -> tuple[int, int, int]:
    """按照 ABI 解析 64 位槽位。"""
    event_type = (value >> 56) & 0xFF
    lap_number = (value >> 52) & 0xF
    address = value & ((1 << 48) - 1)
    return event_type, lap_number, address


def slot_to_row(index: int, value: int) -> List[str]:
    if value == MONITOR_READY_MAGIC:
        return [
            index,
            "Sentinel",
            "kMonitorReady",
            "-",
            "-",
            "-",
            f"0x{value:016x}",
        ]

    if value == PROGRAM_ENDED_MAGIC:
        return [
            index,
            "Sentinel",
            "kProgramEnded",
            "-",
            "-",
            "-",
            f"0x{value:016x}",
        ]

    event_type, lap_number, address = parse_slot(value)

    if event_type in ALLOWED_EVENT_TYPES and event_type != 0:
        name = EVENT_TYPE_NAMES.get(event_type, f"Unknown({event_type})")
        return [
            index,
            "Event",
            name,
            f"0x{event_type:02x}",
            lap_number,
            f"0x{address:012x}",
            f"0x{value:016x}",
        ]

    # 参数槽：不解析 lap/address，仅显示原始值。
    return [
        index,
        "Arg",
        "kEventClear",
        f"0x{event_type:02x}",
        "-",
        "-",
        f"0x{value:016x}",
    ]


def iter_slots(data: bytes) -> Iterable[int]:
    remainder = len(data) % 8
    if remainder:
        trimmed = len(data) - remainder
        data = data[:trimmed]

    for offset in range(0, len(data), 8):
        chunk = data[offset : offset + 8]
        # 通道内是小端写入的，直接以小端解包即可。
        (value,) = struct.unpack_from("<Q", chunk)
        yield value


def render_table(path: Path) -> str:
    data = path.read_bytes()
    rows = [slot_to_row(idx, value) for idx, value in enumerate(iter_slots(data))]
    headers = ["Index", "Slot Type", "Event Name", "EventType", "Lap", "Address", "Raw"]
    return tabulate(rows, headers=headers, tablefmt="grid")


def main() -> None:
    parser = argparse.ArgumentParser(description="打印 TSan monitor 通道内容")
    parser.add_argument("channel", type=Path, help="/tmp/tsan.monitor.<pid>/<tid>" )
    args = parser.parse_args()

    print(render_table(args.channel))


if __name__ == "__main__":
    main()
