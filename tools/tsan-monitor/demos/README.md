# TSan Monitor Demos

该目录收录两个最小化示例：

- `ww_demo.cc`：两个线程并发写同一变量，制造 W/W 数据竞争；
- `rw_demo.cc`：一个线程写、一个线程读同一变量，制造 R/W 数据竞争。

`run_demo.py` 脚本可一键完成以下步骤：
1. 使用指定的 clang/clang++（默认从 `PATH` 查找）并开启 `-fsanitize=thread` 生成 LLVM IR；
2. 构建可执行文件；
3. 通过 `TSAN_OPTIONS=monitor_path=<tsan-monitor>` 运行 demo，让运行时启动外部 monitor；
4. 复制 `/tmp/tsan.monitor.<pid>/` 下的通道文件到输出目录；
5. 调用 `scripts/print_channel.py` 生成可读表格，保存到 `visualized/`。

示例命令：
```bash
python3 run_demo.py \
  --demo ww_demo \
  --monitor ../../build/bin/tsan-monitor \
  --out ../../build/demo-artifacts/ww
```
如需指定 Homebrew LLVM，可额外提供 `--clang` 与 `--clangxx`。

> **注意**：脚本依赖 `tabulate`。在运行前请确保 `pip install tabulate` 已安装。
