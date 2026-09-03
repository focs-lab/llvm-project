#!/bin/bash
# Per-commit gate: 12-config lit matrix, then full check-tsan.
ROOT=${TSAN_ROOT:-$(git -C "$(dirname "$0")" rev-parse --show-toplevel)}; D=$ROOT/llvm/build/projects/compiler-rt/test/tsan/X86_64Config; LIT=$ROOT/llvm/build/bin/llvm-lit
ALLS="-tsan-use-escape-analysis-global -tsan-use-lock-ownership -tsan-use-single-threaded -tsan-use-swmr"
echo "=== 12-config lit matrix on $(git -C $ROOT rev-parse --short=12 HEAD) + working tree ==="
for cfg in "stock:" "EA:-tsan-use-escape-analysis-global" "LO:-tsan-use-lock-ownership" "STC:-tsan-use-single-threaded" "SWMR:-tsan-use-swmr" "DE:-tsan-use-dominance-analysis" "DE+peel:-tsan-use-dominance-analysis -tsan-use-loop-peeling" "sound-only:$ALLS" "AllOpt-peel:$ALLS -tsan-use-dominance-analysis" "AllOpt-peel+DynSTC:$ALLS -tsan-use-dominance-analysis -tsan-use-active-thread-count" "AllOpt+peel:$ALLS -tsan-use-dominance-analysis -tsan-use-loop-peeling" "AllOpt+peel+DynSTC:$ALLS -tsan-use-dominance-analysis -tsan-use-loop-peeling -tsan-use-active-thread-count"; do
  n=${cfg%%:*}; f=${cfg#*:}; out=$(TSAN_MLLVM_FLAGS="$f" timeout 900 $LIT -s $D 2>&1); p=$(echo "$out" | grep -oE "Passed +: +[0-9]+" | grep -oE "[0-9]+"); fl=$(echo "$out" | grep -oE "Failed +: +[0-9]+" | grep -oE "[0-9]+"); names=$(echo "$out" | grep -oE "^FAIL: [^ ]+ :: [^ ]+" | awk '{print $NF}' | tr '\n' ' ')
  printf "  %-20s %s/%s %s\n" "$n" "$p" "${fl:-0}" "$names"; done
echo "=== check-tsan ==="; (cd $ROOT/llvm/build && timeout 2400 /usr/bin/ninja -j56 check-tsan 2>&1 | grep -E "^  (Passed|Failed|Unsupported|Expectedly)|^FAIL:" )
echo GATE_DONE
