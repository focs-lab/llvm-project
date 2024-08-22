//===-- psan_flags.cpp ----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of PredictiveSanitizer (PSan), a predictive race detector.
// This is a fork of ThreadSanitizer (TSan) at LLVM commit
// c609043dd00955bf177ff57b0bad2a87c1e61a36.
//
//===----------------------------------------------------------------------===//

#include "psan_flags.h"

#include "sanitizer_common/sanitizer_flag_parser.h"
#include "sanitizer_common/sanitizer_flags.h"
#include "sanitizer_common/sanitizer_libc.h"
#include "psan_interface.h"
#include "psan_mman.h"
#include "psan_rtl.h"
#include "ubsan/ubsan_flags.h"

namespace __psan {

// Can be overriden in frontend.
#ifdef PSAN_EXTERNAL_HOOKS
extern "C" const char *__psan_default_options();
#else
SANITIZER_WEAK_DEFAULT_IMPL
const char *__psan_default_options() {
  return "";
}
#endif

void Flags::SetDefaults() {
#define PSAN_FLAG(Type, Name, DefaultValue, Description) Name = DefaultValue;
#include "psan_flags.inc"
#undef PSAN_FLAG
  // DDFlags
  second_deadlock_stack = false;
}

void RegisterPsanFlags(FlagParser *parser, Flags *f) {
#define PSAN_FLAG(Type, Name, DefaultValue, Description) \
  RegisterFlag(parser, #Name, Description, &f->Name);
#include "psan_flags.inc"
#undef PSAN_FLAG
  // DDFlags
  RegisterFlag(parser, "second_deadlock_stack",
      "Report where each mutex is locked in deadlock reports",
      &f->second_deadlock_stack);
}

void InitializeFlags(Flags *f, const char *env, const char *env_option_name) {
  SetCommonFlagsDefaults();
  {
    // Override some common flags defaults.
    CommonFlags cf;
    cf.CopyFrom(*common_flags());
    cf.external_symbolizer_path = GetEnv("PSAN_SYMBOLIZER_PATH");
    cf.allow_addr2line = true;
    if (SANITIZER_GO) {
      // Does not work as expected for Go: runtime handles SIGABRT and crashes.
      cf.abort_on_error = false;
      // Go does not have mutexes.
      cf.detect_deadlocks = false;
    }
    cf.print_suppressions = false;
    cf.stack_trace_format = "    #%n %f %S %M";
    cf.exitcode = 66;
    cf.intercept_tls_get_addr = true;
    OverrideCommonFlags(cf);
  }

  f->SetDefaults();

  FlagParser parser;
  RegisterPsanFlags(&parser, f);
  RegisterCommonFlags(&parser);

#if PSAN_CONTAINS_UBSAN
  __ubsan::Flags *uf = __ubsan::flags();
  uf->SetDefaults();

  FlagParser ubsan_parser;
  __ubsan::RegisterUbsanFlags(&ubsan_parser, uf);
  RegisterCommonFlags(&ubsan_parser);
#endif

  // Let a frontend override.
  parser.ParseString(__psan_default_options());
#if PSAN_CONTAINS_UBSAN
  const char *ubsan_default_options = __ubsan_default_options();
  ubsan_parser.ParseString(ubsan_default_options);
#endif
  // Override from command line.
  parser.ParseString(env, env_option_name);
#if PSAN_CONTAINS_UBSAN
  ubsan_parser.ParseStringFromEnv("UBSAN_OPTIONS");
#endif

  // Check flags.
  if (!f->report_bugs) {
    f->report_thread_leaks = false;
    f->report_destroy_locked = false;
    f->report_signal_unsafe = false;
  }

  InitializeCommonFlags();

  if (Verbosity()) ReportUnrecognizedFlags();

  if (common_flags()->help) parser.PrintFlagDescriptions();

  if (f->io_sync < 0 || f->io_sync > 2) {
    Printf("ThreadSanitizer: incorrect value for io_sync"
           " (must be [0..2])\n");
    Die();
  }
}

}  // namespace __psan
