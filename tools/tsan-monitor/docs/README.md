> Base version
>
> - llvm-project: `29ed6000d21e`
> - V8: `b595bf35aca`

## How to run

### Env

- Docker on MacBook (M4 Pro, ARM), Ubuntu 24.04
- FOCS Server (X86), Ubuntu 24.04

### Build

In Docker (ARM)

```bash
# Pull source code
cd /work && git clone -b user/zengyan/monitor https://github.com/focs-lab/llvm-project.git llvm-project-focs
# Build and compile llvm
cmake -S llvm \
      -B build \
      -G Ninja \
      -DCMAKE_C_COMPILER=clang \
      -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_BUILD_TYPE=Release \
      -DLLVM_ENABLE_PROJECTS="clang" \
      -DLLVM_ENABLE_RUNTIMES="compiler-rt;openmp;libcxx;libcxxabi;libunwind" \
      -DLIBOMP_OMPT_SUPPORT=ON \
   -DBUILD_SHARED_LIBS=OFF \
      -DLLVM_BINUTILS_INCDIR=/usr/include \
      -DCMAKE_C_COMPILER_LAUNCHER=ccache \
      -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
      -DLLVM_TARGETS_TO_BUILD=AArch64 \
   -DCOMPILER_RT_INCLUDE_TESTS=ON
ninja -C build -j 8

# Build and compile monitor
cmake -S tools/tsan-monitor \
   -B build/tsan-monitor \
   -G Ninja
cmake --build build/tsan-monitor -- -j 8
```

In FOCS Server (X86)

```bash
# Pull source code
cd ~ && git clone -b user/zengyan/monitor https://github.com/focs-lab/llvm-project.git llvm-project-focs
# Build and compile llvm
cmake -S llvm \
      -B build \
      -G Ninja \
      -DCMAKE_C_COMPILER=clang \
      -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_BUILD_TYPE=Release \
      -DLLVM_ENABLE_PROJECTS="clang" \
      -DLLVM_ENABLE_RUNTIMES="compiler-rt;openmp;libcxx;libcxxabi;libunwind" \
      -DLIBOMP_OMPT_SUPPORT=ON \
   -DBUILD_SHARED_LIBS=OFF \
      -DLLVM_BINUTILS_INCDIR=/usr/include \
      -DCMAKE_C_COMPILER_LAUNCHER=ccache \
      -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
      -DLLVM_TARGETS_TO_BUILD=X86 \
   -DCOMPILER_RT_INCLUDE_TESTS=ON
ninja -C build -j 96

# Build and compile monitor
cmake -S tools/tsan-monitor \
   -B build/tsan-monitor \
   -G Ninja
cmake --build build/tsan-monitor -- -j 16
```

### Compile a program and run with monitor

> Assume: we are using Docker on MacBook (ARM), and clang++ was built in `/work/llvm-project-focs/build/bin/`, monitor was built in `/work/llvm-project-focs/build/tsan-monitor/`

We are going to compile `llvm-project/tools/tsan-monitor/example/dr_rw.cpp` file

```cpp
// Test: Read-Write race on a shared int  
// Category: Basic Race  
// Expectation: RACE  
// Notes: One thread writes while another reads without synchronization  
#include <pthread.h>  
  
int x = 0;  
int r = 0;  
  
static void* writer(void*) {  
  x = 42;  
  return nullptr;  
}  
  
static void* reader(void*) {  
  r = x;  
  return nullptr;  
}  
  
int main() {  
  pthread_t t1;  
  pthread_t t2;  
  pthread_create(&t1, nullptr, writer, nullptr);  
  pthread_create(&t2, nullptr, reader, nullptr);  
  pthread_join(t1, nullptr);  
  pthread_join(t2, nullptr);  
  return 0;  
}
```

> Or you can also just use the Makefile in `llvm-project/tools/tsan-monitor/example/Makefile` to compile this file

```bash
# Compile using our own clang++
/work/llvm-project-focs/build/bin/clang++ \
 -std=c++11 \
 -pthread \
 -fsanitize=thread -g \
 /work/llvm-project-focs/tools/tsan-monitor/example/dr_rw.cpp -o \
 /work/llvm-project-focs/tools/tsan-monitor/example/dr_rw
# Run with monitor
TSAN_OPTIONS=monitor_path=/work/llvm-project-focs/build/tsan-monitor/tsan-monitor:exit_on_race=1:atexit_sleep_ms=500:monitor_verbose=1 \
/work/llvm-project-focs/tools/tsan-monitor/example/dr_rw
# You can see the result like below
StartMonitor
[+] Waiting for monitor to get ready
[+] Monitor Ready
[+] Race detection signal handler installed
[+] Starting race detection wait window (500 ms total)
==================
WARNING: ThreadSanitizer: data race
Race detected: Read-Write
Address        : 0xaaaacac23520
First  Thread  : tid=1 clock=2 write value=0x2a
Second Thread  : tid=2 clock=2 read value=0x2a
Conflict       : thread 1 write conflicts with thread 2 read on the same address.
SUMMARY: ThreadSanitizer: data race (Thread-1 with Thread-2)
==================
[+] Race detected via sentinel file during wait window! (elapsed 40 ms)
[+] Race detected via sentinel file, exit now! (exit_on_race=1)
```

About the env variables to monitor:

monitor_path

- Purpose: Specifies the absolute path to the tsan-monitor executable
- Default: Empty string (monitoring disabled)
- Function: Runtime forks a child process to launch the external monitor program

exit_on_race

- Purpose: Controls program behavior when race is detected
- Values: 1 = exit immediately on race detection, 0 = continue execution
- Function: Enables race-triggered program termination via signal/sentinel file mechanism

atexit_sleep_ms

- Purpose: Wait time in milliseconds before program exit
- Default: 1000ms
- Function: Creates a polling window at exit to catch "at-exit" data races; polls every 10ms for race signals

monitor_verbose

- Purpose: Enable verbose logging for monitor-mode runtime communication
- Default: false
- Function: Controls debug output (MDPrintf) showing monitor startup, waiting, and communication details

## How to test

### Functional test (DEBUG)

There are two ways to run functional tests

1. Use the `example/run.py`

`run.py` will compile a specific program, generate the IR file, run it and use `scripts/print_channel.py` to print the human-readable channel file, which is usually used to debug. After executing, you can check the output file under `example/out/xxx/` folder

The supported examples are in `SUPPORTED_EXAMPLE` list, and the config infos are in `DEFAULT_CONFIG` list which contains clang++ path, monitor path and etc

For example

```bash
# make sure you have load the python env
# source .container-venv/bin/active
cd llvm-project-focs/tools/tsan-monitor/example
python3 run.py -v -e dr_rw
================================================================================
TSAN MONITOR EXAMPLE EXECUTION
================================================================================
Example: dr_rw
Clang binary: /work/llvm-project-focs/build/bin/clang
Clang++ binary: /work/llvm-project-focs/build/bin/clang++
Monitor binary: /work/llvm-project-focs/build/tsan-monitor/tsan-monitor
Output directory: /work/llvm-project-focs/tools/tsan-monitor/example/out/dr_rw
Verbose mode: ON
================================================================================


[+] Generating LLVM IR: /work/llvm-project-focs/build/bin/clang++ -fsanitize=thread -g -S -emit-llvm /work/llvm-project-focs/tools/tsan-monitor/example/dr_rw.cpp -o /work/llvm-project-focs/tools/tsan-monitor/example/out/dr_rw/dr_rw.ll -pthread
[+] Building executable: /work/llvm-project-focs/build/bin/clang++ -fsanitize=thread -g /work/llvm-project-focs/tools/tsan-monitor/example/dr_rw.cpp -o /work/llvm-project-focs/tools/tsan-monitor/example/out/dr_rw/dr_rw -pthread
[+] Running example, waiting for monitor to exit (TSAN_OPTIONS: monitor_path=/work/llvm-project-focs/build/tsan-monitor/tsan-monitor:exit_on_race=1:atexit_sleep_ms=500:monitor_verbose=1)
[+] Example program exited with code True
================================================================================
PROGRAM EXECUTION RESULT:
--------------------------------------------------------------------------------
StartMonitor
[+] Waiting for monitor to get ready
[+] Monitor Ready
[+] Race detection signal handler installed
[+] Starting race detection wait window (500 ms total)
==================
WARNING: ThreadSanitizer: data race
Race detected: Read-Write
Address        : 0xaaaad6fc3520
First  Thread  : tid=1 clock=2 write value=0x2a
Second Thread  : tid=2 clock=2 read value=0x2a
Conflict       : thread 1 write conflicts with thread 2 read on the same address.
SUMMARY: ThreadSanitizer: data race (Thread-1 with Thread-2)
==================
[+] Race detected via sentinel file during wait window! (elapsed 30 ms)
[+] Race detected via sentinel file, exit now! (exit_on_race=1)

================================================================================


[+] Processing /tmp/tsan.monitor.4087/0 with print_channel.py
[+] Event-based table saved to: /work/llvm-project-focs/tools/tsan-monitor/example/out/dr_rw/channel/channel_0.event

[+] Processing /tmp/tsan.monitor.4087/1 with print_channel.py
[+] Event-based table saved to: /work/llvm-project-focs/tools/tsan-monitor/example/out/dr_rw/channel/channel_1.event

[+] Processing /tmp/tsan.monitor.4087/2 with print_channel.py
[+] Event-based table saved to: /work/llvm-project-focs/tools/tsan-monitor/example/out/dr_rw/channel/channel_2.event

[+] Processing /tmp/tsan.monitor.4087/race_found with print_channel.py
[+] Event-based table saved to: /work/llvm-project-focs/tools/tsan-monitor/example/out/dr_rw/channel/channel_unknown.event

================================================================================
EXECUTION SUMMARY:
================================================================================
IR output: /work/llvm-project-focs/tools/tsan-monitor/example/out/dr_rw/dr_rw.ll
Executable: /work/llvm-project-focs/tools/tsan-monitor/example/out/dr_rw/dr_rw
Channel reports: /work/llvm-project-focs/tools/tsan-monitor/example/out/dr_rw/channel
================================================================================
# then you can see folder like below
tree out/dr_rw/
out/dr_rw/
|-- channel
|   |-- channel_0.event
|   |-- channel_1.event
|   |-- channel_2.event
|   `-- channel_unknown.event
|-- dr_rw
|-- dr_rw.ll
|-- report
|   `-- Race_Read-Write_1762315924783_0001.txt
`-- tsan-monitor.log
```

- `tsan-monitor.log` is the log file from monitor, which uses `spdlog`
- `report/` contains the race report file
- `channel/` contains the human-readable channel event files
- `dr_rw.ll` is the generated IR file

2. Use the `pytest` command based on `test/`

`test/` is the tool to easily run multi test/example. Please refer to `llvm-project-focs/tools/tsan-monitor/test/README.md` for specific usage

For example

```bash
# make sure you have load the python env
# source .container-venv/bin/active
cd llvm-project-focs/tools/tsan-monitor/test
pytest -v -n 4 -m should_have_race
========================================================================================================================== test session starts ==========================================================================================================================
platform linux -- Python 3.12.3, pytest-8.4.2, pluggy-1.6.0 -- /work/llvm-project-focs/tools/tsan-monitor/.container-venv/bin/python3
cachedir: .pytest_cache
rootdir: /work/llvm-project-focs
configfile: pyproject.toml
plugins: xdist-3.8.0
4 workers [16 items]
scheduling tests via LoadScheduling

test_examples.py::TestExamples::test_should_detect_race[write_write_race]
test_examples.py::TestExamples::test_should_detect_race[struct_race]
test_examples.py::TestExamples::test_should_detect_race[bank_account_race]
test_examples.py::TestExamples::test_should_detect_race[array_race]
[gw3] [  6%] PASSED test_examples.py::TestExamples::test_should_detect_race[bank_account_race]
test_examples.py::TestExamples::test_should_detect_race[linked_list_race]
[gw2] [ 12%] PASSED test_examples.py::TestExamples::test_should_detect_race[array_race]
test_examples.py::TestExamples::test_should_detect_race[lazy_init_race]
[gw0] [ 18%] PASSED test_examples.py::TestExamples::test_should_detect_race[write_write_race]
[gw1] [ 25%] PASSED test_examples.py::TestExamples::test_should_detect_race[struct_race]
test_examples.py::TestExamples::test_should_detect_race[read_write_race]
test_examples.py::TestExamples::test_should_detect_race[shared_counter_race]
[gw3] [ 31%] PASSED test_examples.py::TestExamples::test_should_detect_race[linked_list_race]
test_examples.py::TestExamples::test_should_detect_race[producer_consumer_race]
[gw0] [ 37%] PASSED test_examples.py::TestExamples::test_should_detect_race[read_write_race]
test_examples.py::TestExamples::test_should_detect_race[wrong_mutex_usage]
[gw2] [ 43%] PASSED test_examples.py::TestExamples::test_should_detect_race[lazy_init_race]
[gw1] [ 50%] PASSED test_examples.py::TestExamples::test_should_detect_race[shared_counter_race]
test_examples.py::TestExamples::test_should_detect_race[mutex_missing_lock]
test_examples.py::TestExamples::test_should_detect_race[atomic_relaxed_race]
[gw3] [ 56%] PASSED test_examples.py::TestExamples::test_should_detect_race[producer_consumer_race]
test_examples.py::TestExamples::test_should_detect_race[no_join_race]
[gw0] [ 62%] PASSED test_examples.py::TestExamples::test_should_detect_race[wrong_mutex_usage]
test_examples.py::TestExamples::test_should_detect_race[nested_join_race]
[gw2] [ 68%] PASSED test_examples.py::TestExamples::test_should_detect_race[mutex_missing_lock]
test_examples.py::TestExamples::test_should_detect_race[partial_join_race]
[gw1] [ 75%] PASSED test_examples.py::TestExamples::test_should_detect_race[atomic_relaxed_race]
test_examples.py::TestExamples::test_should_detect_race[diamond_fork_join]
[gw3] [ 81%] PASSED test_examples.py::TestExamples::test_should_detect_race[no_join_race]
[gw0] [ 87%] PASSED test_examples.py::TestExamples::test_should_detect_race[nested_join_race]
[gw1] [ 93%] PASSED test_examples.py::TestExamples::test_should_detect_race[diamond_fork_join]
[gw2] [100%] PASSED test_examples.py::TestExamples::test_should_detect_race[partial_join_race]

========================================================================================================================== 16 passed in 9.99s ===========================================================================================================================
```

### Benchmark

#### V8

> Daniel's notes about v8: [Chromium/V8 - HackMD](https://hackmd.io/@tsaninternals/Hy9L3J8KA/%2F1pjMkN2mSBaull8ylWgcbg)

The V8 engine is Google’s high-performance JavaScript and WebAssembly engine, originally developed for the Chrome browser and now widely used in environments like Node.js. It translates JavaScript directly into optimized machine code using a just-in-time (JIT) compiler, significantly improving execution speed

To obtain comprehensive performance data, I used three industry-standard JavaScript benchmark suites provided by the V8 engine: Sun- Spider, Kraken, and Octane

NOTE: I only tried on FOCS server, cause V8's benchmark suites are too big to compile on my MacBook (could cost couple days...)

Step 1: Get V8's source code (Ref. [Checking out the V8 source code · V8](https://v8.dev/docs/source-code))

```bash
# Install depot_tools
git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git
export PATH=/path/to/depot_tools:$PATH # add depot_tools to your PATH

# Update depot_tools
gclient

# Get V8 source code
mkdir ~/v8
cd ~/v8
fetch v8
cd v8

# NOTE: we use `b595bf35aca` as the base version of v8, if you have more recent commit, you should checkout to `b595bf35aca` instead
```

Step 2: Get and build Chromium's LLVM

```bash
# Get and build Chromium's source code of LLVM
cd ~/v8/v8
tools/clang/scripts/build.py --without-android --without-fuchsia --with-ccache --disable-asserts
ninja -C third_party/llvm-build/Release+Asserts/ -j96
```

When you executed `build.py`, it will clone the Chromium's LLVM source code to `~/v8/v8/third_party/llvm` and compile necessary lib/tools (not only the compiler clang/clang++). The build folder is `~/v8/v8/third_party/llvm-build/Release+Asserts/`

If you update the source code in `~/v8/v8/third_party/llvm`, you should re-build it using `ninja -C third_party/llvm-build/Release+Asserts/ -j96`, and then re-build v8 though

Sometimes, if some errors occur during building v8, you may need to clean the LLVM build folder first, and re-build the whole LLVM like below:

```bash
# If some dependencies broke (not only clang/clang++), you should clean and re-build the whole LLVM before re-build v8
cd ~/v8/v8
ninja -C third_party/llvm-build/Release+Asserts/ -t clean
ninja -C third_party/llvm-build/Release+Asserts/ -j96
```

Step 3: Build V8 using the base LLVM (Ref. [Building V8 with GN · V8](https://v8.dev/docs/build-gn))

Before you build v8 without TSan, you should update some files about v8, according to Daniel's [note](https://hackmd.io/@tsaninternals/Hy9L3J8KA/%2F1pjMkN2mSBaull8ylWgcbg#Build-V8)

```bash
# Build V8 without tsan
cd ~/v8/v8
gn args out/no-tsan # which could open an editor, you can set the build gn args like below
ninja -C out/no-tsan -j64 d8
```

Building gn args:

```bash
# Build arguments go here.
# See "gn args <out_dir> --list" for available build arguments.

dcheck_always_on = false
target_cpu = "x64"
is_component_build = false
is_debug = false
v8_enable_google_benchmark = true
v8_enable_test_features = true
v8_enable_fast_torque = true

is_tsan = false

# this should be false if you didn't get LLVM in the way described above
clang_use_chrome_plugins = false
# the full path must be specified here, i.e. cannot use ~/
clang_base_path = "/home/zengyan/v8/v8/third_party/llvm-build/Release+Asserts/"
```

- `is_tsan`: set to true if you want to build v8 with tsan
- `clang_base_path`: set to the full path of the built LLVM

Then you should build v8 with tsan

```bash
# Build V8 with tsan
cd ~/v8/v8
gn args out/focs-tsan # make sure you update the `is_tsan` arg to true
ninja -C out/focs-tsan -j64 d8
```

Step 4: Build V8 using our own version of LLVM, aka. monitor

First, you should update the source code of LLVM, aka. replace the code in `~/v8/v8/third_party/llvm` directory with your code

Then, re-build the whole LLVM like below

```bash
cd ~/v8/v8
ninja -C third_party/llvm-build/Release+Asserts/ -t clean
ninja -C third_party/llvm-build/Release+Asserts/ -j96
```

Then, build v8 with tsan and monitor

```bash
cd ~/v8/v8
gn args out/focs-tsan-monitor # make sure you update the `is_tsan` arg to true
ninja -C out/focs-tsan-monitor -j64 d8
```

So now, you have three version of v8 like below

```bash
ll ~/v8/v8/out/
...
drwxrwxr-x  4 zengyan zengyan 4096 Oct 26 13:52 focs-tsan/
drwxrwxr-x  4 zengyan zengyan 4096 Oct 26 19:16 focs-tsan-monitor/
drwxrwxr-x  4 zengyan zengyan 4096 Feb 10  2025 no-tsan/
```

Step 5: Run the test suite of v8

> WARNING: make sure you open the bench-mode of FOCS server like this `bench -c 32 -m 32G` (Ref. [Benchmarking Guide - HackMD](https://hackmd.io/@focs-lab/Sy5IfTNt0/%2F0PoQwv4BToO8OqH4YcwQeA))

1. kraken

```bash
test/benchmarks/csuite/csuite.py kraken baseline out/no-tsan/d8
test/benchmarks/csuite/csuite.py kraken compare out/focs-tsan/d8
                               benchmark:    score |  master_ |      % |
===================================================+==========+========+
                           ai-astar-orig:    202.6 |    114.8 S  -43.3 |
               audio-beat-detection-orig:    170.9 S     73.0 S  -57.3 |
                          audio-dft-orig:    331.5 S     95.2 S  -71.3 |
                          audio-fft-orig:    100.3 S     57.1 |  -43.1 |
                   audio-oscillator-orig:    289.0 |     76.9 |  -73.4 |
              imaging-gaussian-blur-orig:    249.1 |    177.8 |  -28.6 |
                   imaging-darkroom-orig:    231.4 |    166.2 |  -28.2 |
                 imaging-desaturate-orig:    120.7 |     89.1 |  -26.2 |
               json-parse-financial-orig:    616.9 |     46.4 |  -92.5 |
           json-stringify-tinderbox-orig:    460.2 |     30.1 |  -93.5 |
                stanford-crypto-aes-orig:    228.2 S     80.6 |  -64.7 |
                stanford-crypto-ccm-orig:    525.2 |     93.4 |  -82.2 |
             stanford-crypto-pbkdf2-orig:    255.2 S     78.4 |  -69.3 |
   stanford-crypto-sha256-iterative-orig:    122.7 S     33.8 |  -72.5 |
                                  Kraken:   3903.9 |   1212.6 |  -68.9 |
---------------------------------------------------+----------+--------+
test/benchmarks/csuite/csuite.py kraken compare out/focs-tsan-monitor/d8
                               benchmark:    score |  master_ |      % |
===================================================+==========+========+
                           ai-astar-orig:    150.7 |    114.8 S  -23.8 |
               audio-beat-detection-orig:    103.6 S     73.0 S  -29.5 |
                          audio-dft-orig:    212.5 S     95.2 S  -55.2 |
                          audio-fft-orig:     66.8 S     57.1 |  -14.5 |
                   audio-oscillator-orig:    159.5 S     76.9 |  -51.8 |
              imaging-gaussian-blur-orig:    218.9 S    177.8 |  -18.8 |
                   imaging-darkroom-orig:    191.3 |    166.2 |  -13.1 |
                 imaging-desaturate-orig:    101.7 |     89.1 |  -12.4 |
               json-parse-financial-orig:    272.0 |     46.4 |  -82.9 |
           json-stringify-tinderbox-orig:    152.8 |     30.1 |  -80.3 |
                stanford-crypto-aes-orig:    114.5 S     80.6 |  -29.6 |
                stanford-crypto-ccm-orig:    230.5 |     93.4 |  -59.5 |
             stanford-crypto-pbkdf2-orig:    145.4 |     78.4 |  -46.1 |
   stanford-crypto-sha256-iterative-orig:     56.4 |     33.8 |  -40.1 |
                                  Kraken:   2176.5 S   1212.6 |  -44.3 |
---------------------------------------------------+----------+--------+
```

2. octane

```bash
test/benchmarks/csuite/csuite.py octane baseline out/no-tsan/d8
test/benchmarks/csuite/csuite.py octane compare out/focs-tsan/d8
                               benchmark:    score |  master_ |      % |
===================================================+==========+========+
                                Richards:  17843.9 |  18162.4 |   -1.8 |
                               DeltaBlue:  35439.1 S  47074.0 S  -24.7 |
                                  Crypto:  22417.0 |  22889.1 |   -2.1 |
                                RayTrace:  43597.4 |  57918.1 |  -24.7 |
                             EarleyBoyer:  23157.7 |  41177.9 S  -43.8 |
                                  RegExp:   2030.0 |   4900.6 |  -58.6 |
                                   Splay:   1430.9 S  23568.2 |  -93.9 |
                            SplayLatency:   3172.6 S  69849.7 |  -95.5 |
                            NavierStokes:  21993.2 S  24407.3 |   -9.9 |
                                   PdfJS:   4590.4 S  33030.6 |  -86.1 |
                                Mandreel:  15994.7 S  25317.8 |  -36.8 |
                         MandreelLatency:   2421.6 |  25762.3 |  -90.6 |
                                 Gameboy:  12152.4 S  45733.9 |  -73.4 |
                                CodeLoad:   1745.6 |  21090.4 |  -91.7 |
                                   Box2D:  22633.7 S  58894.7 |  -61.6 |
                                    zlib:  16715.8 |  40147.0 |  -58.4 |
                              Typescript:   6856.8 |  42448.6 S  -83.8 |
                                  Octane:   8860.3 S  30658.7 |  -71.1 |
---------------------------------------------------+----------+--------+
test/benchmarks/csuite/csuite.py octane compare out/focs-tsan-monitor/d8
                               benchmark:    score |  master_ |      % |
===================================================+==========+========+
                                Richards:  18033.1 |  18162.4 |   -0.7 |
                               DeltaBlue:  45073.3 |  47074.0 S   -4.3 |
                                  Crypto:  22719.2 |  22889.1 |   -0.7 |
                                RayTrace:  53214.3 |  57918.1 |   -8.1 |
                             EarleyBoyer:  33055.8 |  41177.9 S  -19.7 |
                                  RegExp:   3564.4 |   4900.6 |  -27.3 |
                                   Splay:   8550.9 S  23568.2 |  -63.7 |
                            SplayLatency:   9302.1 S  69849.7 |  -86.7 |
                            NavierStokes:  22584.8 S  24407.3 |   -7.5 |
                                   PdfJS:  12069.3 S  33030.6 |  -63.5 |
                                Mandreel:  21806.4 S  25317.8 |  -13.9 |
                         MandreelLatency:   5257.9 |  25762.3 |  -79.6 |
                                 Gameboy:  32950.7 |  45733.9 |  -28.0 |
                                CodeLoad:   4915.6 |  21090.4 |  -76.7 |
                                   Box2D:  45689.7 |  58894.7 |  -22.4 |
                                    zlib:  26692.0 |  40147.0 |  -33.5 |
                              Typescript:  16910.7 S  42448.6 S  -60.2 |
                                  Octane:  16841.7 S  30658.7 |  -45.1 |
---------------------------------------------------+----------+--------+
```

3. sunspider

```bash
cd 
test/benchmarks/csuite/csuite.py sunspider baseline out/no-tsan/d8
test/benchmarks/csuite/csuite.py sunspider compare out/focs-tsan/d8
                               benchmark:    score |  master_ |      % |
===================================================+==========+========+
                       3d-cube-sunspider:     50.4 |      9.8 S  -80.6 |
                      3d-morph-sunspider:     30.2 |      8.0 S  -73.5 |
                   3d-raytrace-sunspider:     57.8 |     11.4 S  -80.3 |
           access-binary-trees-sunspider:      7.9 S      2.4 S  -69.6 |
               access-fannkuch-sunspider:     26.5 S     13.4 S  -49.4 |
                  access-nbody-sunspider:     18.5 S      4.3 S  -76.8 |
                 access-nsieve-sunspider:     13.9 S      6.4 S  -54.0 |
      bitops-3bit-bits-in-byte-sunspider:      8.0 S      1.9 S  -76.2 |
           bitops-bits-in-byte-sunspider:      8.7 S      3.1 S  -64.4 |
            bitops-bitwise-and-sunspider:     12.2 S      3.0 S  -75.4 |
            bitops-nsieve-bits-sunspider:     35.0 |      7.4 S  -78.9 |
         controlflow-recursive-sunspider:      6.7 S      2.6 S  -61.2 |
                    crypto-aes-sunspider:     32.3 S      7.1 S  -78.0 |
                    crypto-md5-sunspider:     22.1 S      4.8 S  -78.3 |
                   crypto-sha1-sunspider:     20.1 |      4.7 S  -76.6 |
             date-format-tofte-sunspider:    105.9 |      9.1 S  -91.4 |
             date-format-xparb-sunspider:     27.7 S      6.9 S  -75.1 |
                   math-cordic-sunspider:     11.4 S      3.8 S  -66.7 |
             math-partial-sums-sunspider:     25.2 S     14.6 S  -42.1 |
            math-spectral-norm-sunspider:      8.0 S      2.3 S  -71.2 |
                    regexp-dna-sunspider:     56.4 |     13.2 S  -76.6 |
                 string-base64-sunspider:     27.3 S      5.1 S  -81.3 |
                  string-fasta-sunspider:     31.2 S      9.1 S  -70.8 |
               string-tagcloud-sunspider:    141.1 |     16.6 S  -88.2 |
            string-unpack-code-sunspider:    187.4 |     21.7 S  -88.4 |
         string-validate-input-sunspider:     24.0 S      7.9 S  -67.1 |
                               SunSpider:    995.9 |    200.5 S  -79.9 |
---------------------------------------------------+----------+--------+
test/benchmarks/csuite/csuite.py sunspider compare out/focs-tsan-monitor/d8
                               benchmark:    score |  master_ |      % |
===================================================+==========+========+
                       3d-cube-sunspider:     22.6 S      9.8 S  -56.6 |
                      3d-morph-sunspider:     18.3 S      8.0 S  -56.3 |
                   3d-raytrace-sunspider:     26.3 S     11.4 S  -56.7 |
           access-binary-trees-sunspider:      5.2 S      2.4 S  -53.8 |
               access-fannkuch-sunspider:     18.3 S     13.4 S  -26.8 |
                  access-nbody-sunspider:      9.1 S      4.3 S  -52.7 |
                 access-nsieve-sunspider:     13.2 S      6.4 S  -51.5 |
      bitops-3bit-bits-in-byte-sunspider:      4.1 S      1.9 S  -53.7 |
           bitops-bits-in-byte-sunspider:      5.3 S      3.1 S  -41.5 |
            bitops-bitwise-and-sunspider:      6.3 S      3.0 S  -52.4 |
            bitops-nsieve-bits-sunspider:     17.4 S      7.4 S  -57.5 |
         controlflow-recursive-sunspider:      4.0 S      2.6 S  -35.0 |
                    crypto-aes-sunspider:     16.2 S      7.1 S  -56.2 |
                    crypto-md5-sunspider:     12.7 S      4.8 S  -62.2 |
                   crypto-sha1-sunspider:     12.2 S      4.7 S  -61.5 |
             date-format-tofte-sunspider:     33.2 S      9.1 S  -72.6 |
             date-format-xparb-sunspider:     14.7 S      6.9 S  -53.1 |
                   math-cordic-sunspider:      6.1 S      3.8 S  -37.7 |
             math-partial-sums-sunspider:     21.2 S     14.6 S  -31.1 |
            math-spectral-norm-sunspider:      4.6 S      2.3 S  -50.0 |
                    regexp-dna-sunspider:     22.4 S     13.2 S  -41.1 |
                 string-base64-sunspider:     11.8 S      5.1 S  -56.8 |
                  string-fasta-sunspider:     17.7 S      9.1 S  -48.6 |
               string-tagcloud-sunspider:     57.0 S     16.6 S  -70.9 |
            string-unpack-code-sunspider:     63.7 S     21.7 S  -65.9 |
         string-validate-input-sunspider:     12.7 S      7.9 S  -37.8 |
                               SunSpider:    456.4 S    200.5 S  -56.1 |
---------------------------------------------------+----------+--------+
```

> NOTE: The above describes how we used v8 to test the performance differences between `tsan-with-monitor` and `tsan`. However, before starting the monitor, we also used v8 to test the performance overhead of different modules of native tsan on the source program. See [docs.google.com/spreadsheets/d/13zhYPYG\_jUu9nZtb8qf9YhhzSdsO5gyhtQ3BsdehF4E/edit?gid=0#gid=0](https://docs.google.com/spreadsheets/d/13zhYPYG_jUu9nZtb8qf9YhhzSdsO5gyhtQ3BsdehF4E/edit?gid=0#gid=0) for details

#### LevelDB

LevelDB is a high-performance, open-source key-value storage library originally developed by Google and written in C++. It provides a persistent ordered map from string keys to string values

NOTE: I only tried on my ARM env, cause compilation of LevelDB needs some lib/tool which require root permission to install on FOCS server **(same as RocksDB)**

> Assume that you have already built clang++ and monitor as above, and clang++ was built in `/work/Course/Capstone/tsan/llvm-project-focs/build/bin/clang++`

Steps to build LevelDB:

```bash
# Pull the source code
cd /work/code && git clone https://github.com/google/leveldb.git
# Build without tsan
cmake -S . -B build -DCMAKE_CXX_STANDARD=17
cmake --build build -- -j8
# Build with tsan
cmake -S . -B build-tsan \
 -DCMAKE_CXX_STANDARD=17 \
 -DCMAKE_C_COMPILER=clang \
 -DCMAKE_CXX_COMPILER=clang++ \
 -DCMAKE_C_COMPILER_LAUNCHER=ccache \
 -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
 -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" \
 -DCMAKE_C_FLAGS="-fsanitize=thread -g"
cmake --build build-tsan -- -j8
# Build with tsan and monitor
cmake -S . -B build-tsan-monitor \
 -DCMAKE_CXX_STANDARD=17 \
 -DCMAKE_C_COMPILER=/work/Course/Capstone/tsan/llvm-project-focs/build/bin/clang \
 -DCMAKE_CXX_COMPILER=/work/Course/Capstone/tsan/llvm-project-focs/build/bin/clang++ \
 -DCMAKE_C_COMPILER_LAUNCHER=ccache \
 -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
 -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" \
 -DCMAKE_C_FLAGS="-fsanitize=thread -g"
cmake --build build-tsan-monitor -- -j8
```

Then we can use the LevelDB's performance benchmark tool `db_bench` to compare the overhead that tsan and tsan-with-monitor brings to LevelDB

`db_bench` is the official performance benchmarking tool included with LevelDB. It's designed to measure and analyze the performance characteristics of LevelDB under various workloads and configurations, it has some key features like below:

Comprehensive Workload Simulation:

- Sequential and random writes
- Sequential and random reads
- Mixed read/write operations
- Concurrent access patterns
- Different data sizes and compression settings

Detailed Performance Metrics:

- Throughput (operations per second)
- Latency distribution (min, max, percentiles)
- Database size and file statistics
- Memory usage and cache hit rates

```bash
# Run benchmark without tsan
./build/db_bench --benchmarks=fillseq,fillrandom,readrandom,readseq --num=100000 --db=/tmp/testdb
LevelDB:    version 1.23
Date:       Sun Oct 26 01:35:53 2025
CPU:        0 *
CPUCache:
Keys:       16 bytes each
Values:     100 bytes each (50 bytes after compression)
Entries:    100000
RawSize:    11.1 MB (estimated)
FileSize:   6.3 MB (estimated)
WARNING: Optimization is disabled: benchmarks unnecessarily slow
WARNING: Assertions are enabled; benchmarks unnecessarily slow
------------------------------------------------
fillseq      :       1.929 micros/op;   57.3 MB/s
fillrandom   :       2.223 micros/op;   49.8 MB/s
readrandom   :       2.974 micros/op; (63223 of 100000 found)
readseq      :       0.259 micros/op;  427.9 MB/s
# Run benchmark with tsan
./build-tsan/db_bench --benchmarks=fillseq,fillrandom,readrandom,readseq --num=100000 --db=/tmp/testdb
LevelDB:    version 1.23
Date:       Sun Oct 26 01:35:59 2025
CPU:        0 *
CPUCache:
Keys:       16 bytes each
Values:     100 bytes each (50 bytes after compression)
Entries:    100000
RawSize:    11.1 MB (estimated)
FileSize:   6.3 MB (estimated)
WARNING: Optimization is disabled: benchmarks unnecessarily slow
WARNING: Assertions are enabled; benchmarks unnecessarily slow
------------------------------------------------
fillseq      :      11.682 micros/op;    9.5 MB/s
fillrandom   :      16.364 micros/op;    6.8 MB/s
readrandom   :      22.328 micros/op; (63223 of 100000 found)
readseq      :       1.620 micros/op;   68.3 MB/s
# Run benchmark with tsan and monitor
./build-tsan-monitor/db_bench --benchmarks=fillseq,fillrandom,readrandom,readseq --num=100000 --db=/tmp/testdb
StartMonitor
[+] No monitor_path options found in TSAN_OPTIONS. Running without monitor.
LevelDB:    version 1.23
Date:       Sun Oct 26 01:36:06 2025
CPU:        0 *
CPUCache:
Keys:       16 bytes each
Values:     100 bytes each (50 bytes after compression)
Entries:    100000
RawSize:    11.1 MB (estimated)
FileSize:   6.3 MB (estimated)
WARNING: Optimization is disabled: benchmarks unnecessarily slow
WARNING: Assertions are enabled; benchmarks unnecessarily slow
------------------------------------------------
fillseq      :       6.291 micros/op;   17.6 MB/s
fillrandom   :       7.433 micros/op;   14.9 MB/s
readrandom   :      13.240 micros/op; (63223 of 100000 found)
readseq      :       1.362 micros/op;   81.2 MB/s
[+] Starting race detection wait window (1000 ms total)
[+] Wait window expired, no race detected
[+] Continuing execution (exit_on_race=1)
```

#### RocksDB

RocksDB is a fork of LevelDB developed by Facebook (Meta) that extends and optimizes LevelDB for modern hardware and production workloads. Specifically, RocksDB introduces features such as: Multi-threaded Compaction, Advanced Compaction Strategies, Memory Management, etc.

> Assume that you have already built clang++ and monitor as above, and clang++ was built in `/work/Course/Capstone/tsan/llvm-project-focs/build/bin/clang++`

Steps to build RocksDB:

```bash
# Pull the source code
cd /work/code && git clone https://github.com/facebook/rocksdb.git
# Install dependencies
sudo apt-get install \
 libgflags-dev \
 libsnappy-dev \
 zlib1g-dev \
 libbz2-dev \
 liblz4-dev \
 libzstd-dev
# Build without tsan
cmake -S . -B build -G Ninja
cmake --build build -- -j8
# Build with tsan
cmake -S . -B build-tsan -G Ninja \
 -DCMAKE_C_COMPILER=clang \
 -DCMAKE_CXX_COMPILER=clang++ \
 -DCMAKE_C_COMPILER_LAUNCHER=ccache \
 -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
 -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" \
 -DCMAKE_C_FLAGS="-fsanitize=thread -g"
cmake --build build-tsan -- -j8
# Build without tsan and monitor
cmake -S . -B build-tsan-monitor -G Ninja \
 -DCMAKE_C_COMPILER=/work/Course/Capstone/tsan/llvm-project-focs/build/bin/clang \
 -DCMAKE_CXX_COMPILER=/work/Course/Capstone/tsan/llvm-project-focs/build/bin/clang++ \
 -DCMAKE_C_COMPILER_LAUNCHER=ccache \
 -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
 -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" \
 -DCMAKE_C_FLAGS="-fsanitize=thread -g"
cmake --build build-tsan-monitor -- -j8
```

Cause RocksDB is the extension of LevelDB, we can also use the same `db_bench` tool (from RocksDB) to compare the overhead that tsan and tsan-with-monitor brings to RocksDB

```bash
# Run benchmark without tsan
./build/db_bench --benchmarks=fillseq,fillrandom,readrandom,readseq --num=100000 --db=/tmp/testdb --compression_type=none
Set seed to 1761442292643353 because --seed was 0
Initializing RocksDB Options from the specified file
Initializing RocksDB Options from command-line flags
Integrated BlobDB: blob cache disabled
RocksDB:    version 10.9.0
Date:       Sun Oct 26 01:31:32 2025
CPU:        0 *
CPUCache:
Keys:       16 bytes each (+ 0 bytes user-defined timestamp)
Values:     100 bytes each (50 bytes after compression)
Entries:    100000
Prefix:    0 bytes
Keys per prefix:    0
RawSize:    11.1 MB (estimated)
FileSize:   6.3 MB (estimated)
Write rate: 0 bytes/second
Read rate: 0 ops/second
Compression: NoCompression
Compression sampling rate: 0
Memtablerep: SkipListFactory
Perf Level: 1
WARNING: Optimization is disabled: benchmarks unnecessarily slow
WARNING: Assertions are enabled; benchmarks unnecessarily slow
------------------------------------------------
Initializing RocksDB Options from the specified file
Initializing RocksDB Options from command-line flags
Integrated BlobDB: blob cache disabled
DB path: [/tmp/testdb]
fillseq      :       8.327 micros/op 120083 ops/sec 0.833 seconds 100000 operations;   13.3 MB/s
Initializing RocksDB Options from the specified file
Initializing RocksDB Options from command-line flags
Integrated BlobDB: blob cache disabled
DB path: [/tmp/testdb]
fillrandom   :      12.020 micros/op 83193 ops/sec 1.202 seconds 100000 operations;    9.2 MB/s
DB path: [/tmp/testdb]
readrandom   :       5.083 micros/op 196720 ops/sec 0.508 seconds 100000 operations;   13.7 MB/s (63148 of 100000 found)

DB path: [/tmp/testdb]
readseq      :       0.978 micros/op 1022436 ops/sec 0.062 seconds 63207 operations;  113.1 MB/s
# Run benchmark with tsan
./build-tsan/db_bench --benchmarks=fillseq,fillrandom,readrandom,readseq --num=100000 --db=/tmp/testdb --compression_type=none
Set seed to 1761442299269069 because --seed was 0
Initializing RocksDB Options from the specified file
Initializing RocksDB Options from command-line flags
Integrated BlobDB: blob cache disabled
RocksDB:    version 10.9.0
Date:       Sun Oct 26 01:31:39 2025
CPU:        0 *
CPUCache:
Keys:       16 bytes each (+ 0 bytes user-defined timestamp)
Values:     100 bytes each (50 bytes after compression)
Entries:    100000
Prefix:    0 bytes
Keys per prefix:    0
RawSize:    11.1 MB (estimated)
FileSize:   6.3 MB (estimated)
Write rate: 0 bytes/second
Read rate: 0 ops/second
Compression: NoCompression
Compression sampling rate: 0
Memtablerep: SkipListFactory
Perf Level: 1
WARNING: Optimization is disabled: benchmarks unnecessarily slow
WARNING: Assertions are enabled; benchmarks unnecessarily slow
------------------------------------------------
Initializing RocksDB Options from the specified file
Initializing RocksDB Options from command-line flags
Integrated BlobDB: blob cache disabled
DB path: [/tmp/testdb]
fillseq      :      54.792 micros/op 18250 ops/sec 5.479 seconds 100000 operations;    2.0 MB/s
Initializing RocksDB Options from the specified file
Initializing RocksDB Options from command-line flags
Integrated BlobDB: blob cache disabled
DB path: [/tmp/testdb]
fillrandom   :      77.570 micros/op 12891 ops/sec 7.757 seconds 100000 operations;    1.4 MB/s
DB path: [/tmp/testdb]
readrandom   :      33.930 micros/op 29472 ops/sec 3.393 seconds 100000 operations;    2.1 MB/s (63252 of 100000 found)

DB path: [/tmp/testdb]
readseq      :       4.734 micros/op 211217 ops/sec 0.300 seconds 63284 operations;   23.4 MB/s
# Run benchmark with tsan and monitor
./build-tsan-monitor/db_bench --benchmarks=fillseq,fillrandom,readrandom,readseq --num=100000 --db=/tmp/testdb --compression_type=none
StartMonitor
[+] No monitor_path options found in TSAN_OPTIONS. Running without monitor.
Set seed to 1761442318101298 because --seed was 0
Initializing RocksDB Options from the specified file
Initializing RocksDB Options from command-line flags
Integrated BlobDB: blob cache disabled
RocksDB:    version 10.9.0
Date:       Sun Oct 26 01:31:58 2025
CPU:        0 *
CPUCache:
Keys:       16 bytes each (+ 0 bytes user-defined timestamp)
Values:     100 bytes each (50 bytes after compression)
Entries:    100000
Prefix:    0 bytes
Keys per prefix:    0
RawSize:    11.1 MB (estimated)
FileSize:   6.3 MB (estimated)
Write rate: 0 bytes/second
Read rate: 0 ops/second
Compression: NoCompression
Compression sampling rate: 0
Memtablerep: SkipListFactory
Perf Level: 1
WARNING: Optimization is disabled: benchmarks unnecessarily slow
WARNING: Assertions are enabled; benchmarks unnecessarily slow
------------------------------------------------
Initializing RocksDB Options from the specified file
Initializing RocksDB Options from command-line flags
Integrated BlobDB: blob cache disabled
DB path: [/tmp/testdb]
fillseq      :      27.208 micros/op 36753 ops/sec 2.721 seconds 100000 operations;    4.1 MB/s
Initializing RocksDB Options from the specified file
Initializing RocksDB Options from command-line flags
Integrated BlobDB: blob cache disabled
DB path: [/tmp/testdb]
fillrandom   :      47.755 micros/op 20939 ops/sec 4.776 seconds 100000 operations;    2.3 MB/s
DB path: [/tmp/testdb]
readrandom   :      21.871 micros/op 45721 ops/sec 2.187 seconds 100000 operations;    3.2 MB/s (63270 of 100000 found)

DB path: [/tmp/testdb]
readseq      :       3.465 micros/op 288528 ops/sec 0.219 seconds 63276 operations;   31.9 MB/s
[+] Starting race detection wait window (1000 ms total)
[+] Wait window expired, no race detected
[+] Continuing execution (exit_on_race=1)
```

### Compilation & Binary/Library size overhead

While the separated monitoring architecture greatly improves runtime performance, it introduces additional cost during the compilation stage. The new instrumentation logic is more complex, eg. each memory access expands from a single function call to 5–8 LLVM instructions, which leading to slower compilation, harder optimization, larger binaries, and higher compiler memory usage.

We only tested LevelDB and RocksDB on ARM env for this part:

```bash
# LevelDB
# Compilation time
time cmake --build build -- -j8 # real 0.766s
time cmake --build build-tsan -- -j8 # real 1.291s
time cmake --build build-tsan-monitor -- -j8 # real 13.999s
# Binary/Library Size
ls -lh build/libleveldb.a build-tsan/libleveldb.a build-tsan-monitor/libleveldb.a
-rw-r--r-- 1 root root 6.9M Nov  6 05:47 build-tsan-monitor/libleveldb.a
-rw-r--r-- 1 root root 6.2M Nov  6 05:47 build-tsan/libleveldb.a
-rw-r--r-- 1 root root 2.1M Nov  6 05:47 build/libleveldb.a
ls -lh build/db_bench build-tsan/db_bench build-tsan-monitor/db_bench
-rwxr-xr-x 1 root root 5.7M Nov  6 05:47 build-tsan-monitor/db_bench
-rwxr-xr-x 1 root root 4.2M Nov  6 05:47 build-tsan/db_bench
-rwxr-xr-x 1 root root 706K Nov  6 05:47 build/db_bench

# RocksDB
# Compilation time
time cmake --build build -- -j8 # real 31.220s
time cmake --build build-tsan -- -j8 # real 43.622s
time cmake --build build-tsan-monitor -- -j8 # real 
# Binary/Library Size
ls -lh build/librocksdb.a build-tsan/librocksdb.a build-tsan-monitor/librocksdb.a
-rw-r--r-- 1 root root 484M Oct 25 15:15 build-tsan-monitor/librocksdb.a
-rw-r--r-- 1 root root 437M Oct 25 15:03 build-tsan/librocksdb.a
-rw-r--r-- 1 root root 385M Oct 23 07:46 build/librocksdb.a
ls -lh build/librocksdb.so.10.9.0 build-tsan/librocksdb.so.10.9.0 build-tsan-monitor/librocksdb.so.10.9.0
-rwxr-xr-x 1 root root 167M Oct 26 01:16 build-tsan-monitor/librocksdb.so.10.9.0
-rwxr-xr-x 1 root root 152M Oct 25 15:05 build-tsan/librocksdb.so.10.9.0
-rwxr-xr-x 1 root root 142M Oct 23 07:48 build/librocksdb.so.10.9.0
ls -lh build/db_bench build-tsan/db_bench build-tsan-monitor/db_bench
-rwxr-xr-x 1 root root  11M Oct 26 01:22 build-tsan-monitor/db_bench
-rwxr-xr-x 1 root root 8.1M Oct 25 15:08 build-tsan/db_bench
-rwxr-xr-x 1 root root 5.8M Oct 23 07:51 build/db_bench
```

### LLVM-LIT

`llvm-lit` (LLVM Integrated Tester) is LLVM’s lightweight and flexible testing framework used to run, manage, and verify test suites across LLVM projects such as Clang and LLDB. It automates regression testing by discovering and executing tests (like `.ll` IR tests and `.c` compiler tests), supports parallel execution, integrates seamlessly with CMake via commands like `ninja check`, and reports detailed results. In short, `llvm-lit` ensures the stability and correctness of the LLVM toolchain through efficient and consistent automated testing.(Ref. [lit - LLVM Integrated Tester — LLVM 22.0.0git documentation](https://llvm.org/docs/CommandGuide/lit.html))

Here are bunch of test cases from llvm-project about tsan. If you want to run these cases using our monitor, you can follow the steps below. But our monitor can not pass all of these test cases, case some cases monitor dose not support yet and most of cases require more specific race report, eg. `llvm-lit` will check whether tsan print out the detailed stack trace infos (but monitor cannot report this now). The only one case that can be passed is `compiler-rt/test/tsan/simple_race.c`

Steps to use `llvm-lit`:

`llvm-lit` needs a specific version of `libc++.a`, which should be built in `build/runtimes/runtimes-bins/compiler-rt/lib/tsan/libcxx_tsan_x86_64/lib/libc++.a`

Build llvm like [above](#build), you can find `llvm-lit` in `build/bin/` folder

Then build the specific `libc++.a`

```bash
ninja -C build/runtimes/runtimes-bins -t targets | grep libcxx_tsan # list targets
ninja -C build/runtimes/runtimes-bins libcxx_tsan -j 96 # build all libcxx_tsan targets 
ninja -C build/runtimes/runtimes-bins libcxx_tsan_x86_64-install-cmake326-workaround -j 96 # or just this one target
```

Then you can run the case

```bash
# FOCS Server (X86)
./build/bin/llvm-lit -sv -j 32 \
  build/runtimes/runtimes-bins/compiler-rt/test/tsan/X86_64Config \
  --filter='simple_race\.cpp$'
./build/bin/llvm-lit -sv -j 32 \
  build/runtimes/runtimes-bins/compiler-rt/test/tsan/X86_64Config

# MacBook (ARM)
./build/bin/llvm-lit -sv -j 8 \
  build/runtimes/runtimes-bins/compiler-rt/test/tsan/AARCH64Config \
  --filter='simple_race\.cpp$'
./build/bin/llvm-lit -sv -j 8 \
  build/runtimes/runtimes-bins/compiler-rt/test/tsan/AARCH64Config

# NOTE: plz do not run all cases at once using tsan-with-monitor, which might hang your program
```

## Others

- [Daniel's work](./Daniels_work.md)
- [Known problems](./Known_problems.md)

