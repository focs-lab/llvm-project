type ^
  psan_go.cpp ^
  ..\rtl\psan_interface_atomic.cpp ^
  ..\rtl\psan_flags.cpp ^
  ..\rtl\psan_md5.cpp ^
  ..\rtl\psan_report.cpp ^
  ..\rtl\psan_rtl.cpp ^
  ..\rtl\psan_rtl_access.cpp ^
  ..\rtl\psan_rtl_mutex.cpp ^
  ..\rtl\psan_rtl_report.cpp ^
  ..\rtl\psan_rtl_thread.cpp ^
  ..\rtl\psan_rtl_proc.cpp ^
  ..\rtl\psan_suppressions.cpp ^
  ..\rtl\psan_sync.cpp ^
  ..\rtl\psan_stack_trace.cpp ^
  ..\rtl\psan_vector_clock.cpp ^
  ..\..\sanitizer_common\sanitizer_allocator.cpp ^
  ..\..\sanitizer_common\sanitizer_common.cpp ^
  ..\..\sanitizer_common\sanitizer_flags.cpp ^
  ..\..\sanitizer_common\sanitizer_stacktrace.cpp ^
  ..\..\sanitizer_common\sanitizer_libc.cpp ^
  ..\..\sanitizer_common\sanitizer_printf.cpp ^
  ..\..\sanitizer_common\sanitizer_suppressions.cpp ^
  ..\..\sanitizer_common\sanitizer_thread_registry.cpp ^
  ..\rtl\psan_platform_windows.cpp ^
  ..\..\sanitizer_common\sanitizer_win.cpp ^
  ..\..\sanitizer_common\sanitizer_deadlock_detector1.cpp ^
  ..\..\sanitizer_common\sanitizer_stack_store.cpp ^
  ..\..\sanitizer_common\sanitizer_stackdepot.cpp ^
  ..\..\sanitizer_common\sanitizer_flag_parser.cpp ^
  ..\..\sanitizer_common\sanitizer_symbolizer.cpp ^
  ..\..\sanitizer_common\sanitizer_termination.cpp ^
  ..\..\sanitizer_common\sanitizer_file.cpp ^
  ..\..\sanitizer_common\sanitizer_symbolizer_report.cpp ^
  ..\..\sanitizer_common\sanitizer_mutex.cpp ^
  ..\rtl\psan_external.cpp ^
  > gopsan.cpp

gcc ^
  -c ^
  -o race_windows_amd64.syso ^
  gopsan.cpp ^
  -I..\rtl ^
  -I..\.. ^
  -I..\..\sanitizer_common ^
  -I..\..\..\include ^
  -m64 ^
  -Wall ^
  -fno-exceptions ^
  -fno-rtti ^
  -DSANITIZER_GO=1 ^
  -DWINVER=0x0600 ^
  -D_WIN32_WINNT=0x0600 ^
  -DGetProcessMemoryInfo=K32GetProcessMemoryInfo ^
  -Wno-error=attributes ^
  -Wno-attributes ^
  -Wno-format ^
  -Wno-maybe-uninitialized ^
  -DSANITIZER_DEBUG=0 ^
  -DSANITIZER_WINDOWS=1 ^
  -O3 ^
  -fomit-frame-pointer ^
  -msse3 ^
  -std=c++17

rem "-msse3" used above to ensure continued support of older
rem cpus (for now), see https://github.com/golang/go/issues/53743.
