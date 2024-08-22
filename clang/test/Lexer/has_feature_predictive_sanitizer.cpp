// RUN: %clang_cc1 -E -fsanitize=predict %s -o - | FileCheck --check-prefix=CHECK-PSAN %s
// RUN: %clang_cc1 -E  %s -o - | FileCheck --check-prefix=CHECK-NO-PSAN %s

#if __has_feature(predictive_sanitizer)
int PredictiveSanitizerEnabled();
#else
int PredictiveSanitizerDisabled();
#endif

// CHECK-TSAN: PredictiveSanitizerEnabled
// CHECK-NO-TSAN: PredictiveSanitizerDisabled
