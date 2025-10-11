# TSan Monitor (Scaffold)

This directory hosts the standalone monitor process for the split ThreadSanitizer prototype.  Stage 0 only wires up
basic project scaffolding so the executable can be built and iterated independently of the full LLVM tree.

## Building

```bash
cmake -S tools/tsan-monitor -B build/tsan-monitor \
      -DCMAKE_BUILD_TYPE=Debug
cmake --build build/tsan-monitor
```

The project targets C++17 and sticks to the standard library plus POSIX/`<unistd.h>` style interfaces.  Future stages
will add the platform-specific pieces needed to mmap the origin channels and run the FastTrack analyzer.

## Running

Stage 1 introduces the basic runtime skeleton:

- `--attach <pid>` is required and points the monitor at `/tmp/tsan.monitor.<pid>` unless `--monitor-dir` overrides.
- A channel reader thread is spawned per `<tid>` file and feeds a per-thread FIFO queue.
- The scheduler thread drains the queues and forwards events to a stub analyzer (currently only prints when
  `--verbose` is supplied).  Seeing the `kProgramEnded` marker will terminate the monitor.

Subsequent stages will populate the analyzer with FastTrack state, add synchronization handling, and upgrade the
reporting pipeline.

## 辅助脚本

`scripts/print_channel.py` 可用于离线检查某个通道文件，将每个 64 位槽位解析为事件/参数并以表格形式输出：

```bash
python3 tools/tsan-monitor/scripts/print_channel.py /tmp/tsan.monitor.<pid>/<tid>
```
