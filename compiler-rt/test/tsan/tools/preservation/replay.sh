#!/bin/bash
# replayv2.sh <cfg> "<mllvm flags>" <K>
# Faithful replay: runs lit for the configuration (regenerating every test's
# .script with those flags), then executes each .script verbatim K times --
# same cwd, env, redirects, RUN lines, %deflake, `not` -- with FileCheck (and
# count) shadowed by a function that passes the text through, so the program
# output every RUN line would have checked is captured instead of judged.
set -u
CFG=$1; FLAGS=$2; K=${3:-5}
ROOT=${TSAN_ROOT:-$(git -C "$(dirname "$0")" rev-parse --show-toplevel)}
D=$ROOT/llvm/build/projects/compiler-rt/test/tsan/X86_64Config
OUT=${REPORTS:-$(dirname "$0")/reports}/$CFG; rm -rf "$OUT"; mkdir -p "$OUT"
export TSAN_OPTIONS=atexit_sleep_ms=0 CLANG_NO_DEFAULT_CONFIG=1 PATH=$ROOT/llvm/build/bin:$PATH
TSAN_MLLVM_FLAGS="$FLAGS" timeout 1200 llvm-lit -s "$D" > "$OUT/lit.log" 2>&1
grep -E "^  (Passed|Failed|Unsupported|Expectedly)|^FAIL:" "$OUT/lit.log" > "$OUT/lit.summary"
FileCheck() { local f=""; while [ $# -gt 0 ]; do case "$1" in --input-file) f="$2"; shift;; --input-file=*) f="${1#--input-file=}";; esac; shift; done; if [ -n "$f" ]; then cat "$f"; else cat; fi; return 0; }
count() { cat; return 0; }
export -f FileCheck count
replay_one() { # <script path>
  sc=$1; dir=$(dirname "$sc"); rel=${sc#$D/}; t=$(echo "${rel%.script}" | sed 's|/Output/|__|; s|^Output/||')
  body=$(sed -e 's/^set -o pipefail;set -x;/set -o pipefail;set +x;/' "$sc")
  for k in $(seq 1 $K); do
    (cd "$dir" && timeout 300 bash -c "$(declare -f FileCheck count); $body") > "$OUT/$t.k$k.out" 2>&1
    echo "### EXIT=$?" >> "$OUT/$t.k$k.out"
  done
  touch "$OUT/$t.cmds"   # marks the test as replayed (diffreports keys on *.cmds)
}
export -f replay_one; export OUT D K
find "$D" -name '*.script' | xargs -P 40 -I{} bash -c 'replay_one "{}"'
echo "replayed $(ls $OUT/*.cmds | wc -l) tests x $K for $CFG"
