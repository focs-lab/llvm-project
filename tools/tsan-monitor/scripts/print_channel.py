#!/usr/bin/env python3
"""Print a single TSan monitor channel file in human-readable format.

This script reads 64-bit values slot by slot and distinguishes between event slots
and parameter slots according to the current ABI:
- Event slots (header slots) parse EventType, Lap, Address fields
- Parameter slots are displayed as raw 64-bit hexadecimal values

Usage:
    python3 print_channel.py /tmp/tsan.monitor.<pid>/<tid>

Dependencies: tabulate (install with `pip install tabulate`)
"""

from __future__ import annotations

import argparse
import struct
import sys
from datetime import datetime
from pathlib import Path
from typing import Dict, Iterable, List, Tuple, Union
from zoneinfo import ZoneInfo

from tabulate import tabulate

# Event type table synchronized with the Origin side
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
    0x14: "kMutexLock",
    0x15: "kMutexUnlock",
    0x19: "kThreadSpawn",   # 25
    0x1A: "kThreadJoin",    # 26
    0x1B: "kThreadExit",    # 27
    0x1C: "kThreadStart",   # 28
    0xFE: "kEventIgnoreBegin",
    0xFF: "kEventIgnoreEnd",
}
ALLOWED_EVENT_TYPES = set(EVENT_TYPE_NAMES.keys())

# Sentinel values for handshake and program termination (non-standard event headers, written directly as raw u64)
MONITOR_READY_MAGIC = 0x00000000CAFEBEEF
PROGRAM_ENDED_MAGIC = 0x00000000DEADDEAD

# Event types that require arguments (based on the analysis of staged changes)
EVENTS_WITH_ARGS = {
    0x01,  # kEventRead (value)
    0x02,  # kEventWrite (value)
    0x04,  # kEventVptrLoad (value)
    0x07,  # kEventAtomicLoad (counter, val, mo)
    0x08,  # kEventAtomicStore (counter, val, mo)
    0x09,  # kEventAtomicRMW (counter, old_val, new_val, mo)
    0x0A,  # kEventAtomicCAS (counter, old_val, new_val, mo, success)
    0x14,  # kMutexLock (counter)
    0x15,  # kMutexUnlock (counter)
    0x19,  # kThreadSpawn (child_tid)
    0x1A,  # kThreadJoin (child_tid)
}


def parse_slot(value: int) -> Tuple[int, int, int]:
    """Parse 64-bit slot according to ABI."""
    event_type = (value >> 56) & 0xFF
    lap_number = (value >> 52) & 0xF
    address = value & ((1 << 48) - 1)
    return event_type, lap_number, address


def iter_slots(data: bytes) -> Iterable[int]:
    """Decode byte data into 64-bit integer sequence."""
    remainder = len(data) % 8
    if remainder:
        data = data[:len(data) - remainder]  # Align to 8 bytes

    for offset in range(0, len(data), 8):
        chunk = data[offset:offset + 8]
        # Channel is written in little-endian, unpack directly with little-endian
        (value,) = struct.unpack_from("<Q", chunk)
        yield value


def slot_to_row(index: int, value: int) -> List[str]:
    """Convert a single slot to table row for detailed slot-based view."""
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

    # Parameter slot: don't parse lap/address, just show raw value
    return [
        index,
        "Arg",
        "-",
        "-",
        "-",
        "-",
        f"0x{value:016x}",
    ]


def parse_events(slots: Iterable[int]) -> List[Dict]:
    """Parse slots into event-centric structure, merging events with their arguments."""
    events = []
    slot_iter = iter(slots)

    for slot_idx, value in enumerate(slot_iter):
        # Handle sentinel values
        if value == MONITOR_READY_MAGIC:
            events.append({
                'slot_index': slot_idx,
                'type': 'sentinel',
                'name': 'kMonitorReady',
                'raw': f"0x{value:016x}"
            })
            continue

        if value == PROGRAM_ENDED_MAGIC:
            events.append({
                'slot_index': slot_idx,
                'type': 'sentinel',
                'name': 'kProgramEnded',
                'raw': f"0x{value:016x}"
            })
            continue

        # Parse event header
        event_type, lap_number, address = parse_slot(value)

        if event_type not in ALLOWED_EVENT_TYPES or event_type == 0:
            # This is a stray argument slot, skip it
            continue

        event_name = EVENT_TYPE_NAMES.get(event_type, f"Unknown({event_type})")

        # Base event information
        event = {
            'slot_index': slot_idx,
            'type': 'event',
            'event_type': f"0x{event_type:02x}",
            'name': event_name,
            'lap': lap_number,
            'address': f"0x{address:012x}" if address != 0 else "-",
            'raw': f"0x{value:016x}",
            'args': []
        }

        # Collect arguments based on event type
        if event_type in EVENTS_WITH_ARGS:
            # Determine number of arguments based on event type
            if event_type in [0x01, 0x02, 0x04]:  # kEventRead/kEventWrite/kEventVptrLoad: optional 1 arg (value)
                # Check if there might be a value argument by looking at the next slot
                try:
                    next_value = next(slot_iter)
                    next_event_type, _, _ = parse_slot(next_value)

                    # If the next slot is not a valid event header, treat it as a value argument
                    if next_event_type not in ALLOWED_EVENT_TYPES or next_event_type == 0:
                        event['args'].append(f"0x{next_value:016x}")
                    else:
                        # Put it back - it's actually the next event
                        remaining_slots = [next_value] + list(slot_iter)
                        slot_iter = iter(remaining_slots)
                except StopIteration:
                    # No more slots, this was the last event
                    pass
            elif event_type in [0x14, 0x15]:  # MutexLock/Unlock: 1 arg (counter)
                arg_count = 1
                for _ in range(arg_count):
                    try:
                        arg_value = next(slot_iter)
                        event['args'].append(f"0x{arg_value:016x}")
                    except StopIteration:
                        break
            elif event_type in [0x19, 0x1A]:  # ThreadSpawn/Join: 1 arg (child_tid)
                arg_count = 1
                for _ in range(arg_count):
                    try:
                        arg_value = next(slot_iter)
                        event['args'].append(f"0x{arg_value:016x}")
                    except StopIteration:
                        break
            elif event_type in [0x07, 0x08]:  # AtomicLoad/Store: 3 args (counter, val, mo)
                arg_count = 3
                for _ in range(arg_count):
                    try:
                        arg_value = next(slot_iter)
                        event['args'].append(f"0x{arg_value:016x}")
                    except StopIteration:
                        break
            elif event_type == 0x09:  # AtomicRMW: 4 args (counter, old_val, new_val, mo)
                arg_count = 4
                for _ in range(arg_count):
                    try:
                        arg_value = next(slot_iter)
                        event['args'].append(f"0x{arg_value:016x}")
                    except StopIteration:
                        break
            elif event_type == 0x0A:  # AtomicCAS: 5 args (counter, old_val, new_val, mo, success)
                arg_count = 5
                for _ in range(arg_count):
                    try:
                        arg_value = next(slot_iter)
                        event['args'].append(f"0x{arg_value:016x}")
                    except StopIteration:
                        break
            else:
                # Default to 1 arg for unknown event types
                try:
                    arg_value = next(slot_iter)
                    event['args'].append(f"0x{arg_value:016x}")
                except StopIteration:
                    pass

        events.append(event)

    return events


def render_slot_table(slots: Iterable[int]) -> str:
    """Render detailed slot-based table view."""
    rows = [slot_to_row(idx, value) for idx, value in enumerate(slots)]
    headers = ["Index", "Slot Type", "Event Name", "EventType", "Lap", "Address", "Raw"]
    return tabulate(rows, headers=headers, tablefmt="grid")


def get_event_args_documentation() -> str:
    """Get documentation for event argument types and counts."""
    return """# Event Arguments Documentation:
# kEventRead (value)
# kEventWrite (value)
# kEventVptrLoad (value)
# kEventAtomicLoad (counter, val, mo)
# kEventAtomicStore (counter, val, mo)
# kEventAtomicRMW (counter, old_val, new_val, mo)
# kEventAtomicCAS (counter, old_val, new_val, mo, success)
# kMutexLock (counter)
# kMutexUnlock (counter)
# kThreadSpawn (child_tid)
# kThreadJoin (child_tid)
"""


def render_event_table(events: List[Dict]) -> str:
    """Render event-centric table view with merged arguments."""
    rows = []
    event_index = 0  # Sequential event counter

    for event in events:
        if event['type'] == 'sentinel':
            rows.append([
                event['slot_index'],
                "-",
                "Sentinel",
                event['name'],
                "-",
                "-"
            ])
        else:  # event
            # Format arguments as comma-separated list
            args_str = ", ".join(event['args']) if event['args'] else "-"

            rows.append([
                event['slot_index'],
                event_index,
                "Event",
                event['name'],
                event['address'],
                args_str
            ])
            event_index += 1

    headers = ["SlotIndex", "EventIndex", "Type", "EventName", "Address", "Args"]
    return tabulate(rows, headers=headers, tablefmt="grid")


def extract_tid_from_path(channel_path: Path) -> str:
    """Extract TID from channel path like /tmp/tsan.monitor.<pid>/<tid>."""
    path_parts = channel_path.parts
    tid = "unknown"

    for part in path_parts:
        if part.isdigit():
            tid = part
            break

    return tid


def main() -> None:
    parser = argparse.ArgumentParser(description="Print TSan monitor channel content")
    parser.add_argument("channel", type=Path, help="/tmp/tsan.monitor.<pid>/<tid>")
    parser.add_argument("--format", choices=["slot", "event", "both"], default="both",
                       help="Output format: slot-based table, event-based table, or both (default)")
    parser.add_argument("--output-dir", default=".", help="Output directory for saved files (default: current directory)")

    args = parser.parse_args()

    if not args.channel.exists():
        print(f"Error: Channel file {args.channel} does not exist", file=sys.stderr)
        sys.exit(1)

    # Read and parse channel data
    data = args.channel.read_bytes()
    slots = list(iter_slots(data))
    events = parse_events(slots)

    # Extract TID for output filenames
    tid = extract_tid_from_path(args.channel)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(exist_ok=True)

    # Generate and save outputs based on requested format
    if args.format in ["slot", "both"]:
        slot_output = render_slot_table(slots)
        slot_filename = output_dir / f"channel_{tid}.slot"
        with slot_filename.open('w') as f:
            now = datetime.now(ZoneInfo("Asia/Shanghai"))
            f.write(f"# Slot-based view for channel {args.channel}\n")
            f.write(f"# Generated at {now}\n\n")
            f.write(get_event_args_documentation())
            f.write("\n")
            f.write(slot_output)
        print(f"[+] Slot-based table saved to: {slot_filename}")

    if args.format in ["event", "both"]:
        event_output = render_event_table(events)
        event_filename = output_dir / f"channel_{tid}.event"
        with event_filename.open('w') as f:
            now = datetime.now(ZoneInfo("Asia/Shanghai"))
            f.write(f"# Event-based view for channel {args.channel}\n")
            f.write(f"# Generated at {now}\n\n")
            f.write(get_event_args_documentation())
            f.write("\n")
            f.write(event_output)
        print(f"[+] Event-based table saved to: {event_filename}")


if __name__ == "__main__":
    main()
