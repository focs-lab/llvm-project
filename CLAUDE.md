# TSan instrumentation-reduction — research repo

This is the **research repo**: the prototype behind the paper in `~/tsan-instr-paper`
(USENIX ATC '26, submission #571). Every number in the paper comes from here.
The paper repo has its own CLAUDE.md — read it before touching the paper.

`~/dev/llvm-project-tsan` (`focs-lab/llvm-project-tsan`) is a **separate repo, for
upstreaming PRs only**: three commits on upstream main (`tsan-escape-analysis`,
`tsan-escape-analysis-integration`, `tsan-dominance-based`). Those are rewrites, not
cleanups of this prototype, and they differ in behaviour — some already fix soundness
holes this tree still has. A claim verified in one tree says nothing about the other;
always state which tree and which branch you tested.

Working branch here: `tsan-with-ea-IPA-dom-fix`. Its sibling `tsan-oracle-10.03.26` is
the same content plus a tracing commit, used for the dynamic metrics (L, M).

## Build and test

- Build dir: `llvm/build` → symlink to `llvm/cmake-build-release` (Release + assertions,
  `clang;lld;compiler-rt`, X86 only, lld, `LLVM_PARALLEL_LINK_JOBS=4`). Configured by
  CLion, but `/usr/bin/ninja -C llvm/build` works.
- 112 cores / 250 GB. Cap at `-j56`, what the experiment scripts already use.
- **Check the build is current before trusting a binary.** `ninja -n | tail -1` shows the
  pending step count; a stale tree silently answers with month-old behaviour.
- IR-level check: `llvm/build/bin/clang -O1 -fsanitize=thread -mllvm <flag> -S -emit-llvm -o -`,
  then grep `__tsan_read`/`__tsan_write`. End-to-end: link and run; the runtime is built.
- Lit: `ninja -C llvm/build check-tsan`, and
  `llvm/build/bin/llvm-lit llvm/test/Instrumentation/ThreadSanitizerNew`.
  Note `compiler-rt/test/tsan/lit.cfg.py` **force-injects** `-mllvm -tsan-use-escape-analysis-global`
  into every TSan test — so `check-tsan` validates EA and nothing else.
- `~/tsan-experiments/tsan-tests/run-tsan-tests.sh` is stale: it points at
  `~/dev/llvm-project-tsan` and at branch/test names that no longer exist.

## Two traps specific to this compiler

- **The analyses read summary files from the current working directory.** `LockOwnership`
  reads and writes `lo_summary.txt` in the CWD and *reuses* it if present — so a build run
  in a directory holding another project's summary silently loads that project's results.
  (`~/tsan-experiments/tools/lo_summary.txt` is one such leftover, holding Redis symbols.)
  `SingleThreaded` and `EscapeAnalysis` write into `tsan-logs/` but read the bare filename,
  so their reuse paths never fire. Always build from a clean directory, and say which one.
- **stderr is not clean.** The module pass prints `-- Using ... Analysis for Module ...`
  unconditionally on every compile. Do not read compiler stderr as a signal.

## A CLEAN result is not evidence until the check is known to fire

Every claim in this project is a **negative**: instrumentation removed, races still found.
Negatives are exactly what a broken check produces for free.

- **Pair every removal with a positive control** — for each analysis, a test where the
  access MUST stay instrumented, differing from the eliminated case by one thing. A
  `CHECK-NOT` with no `CHECK` counterpart proves nothing. The tests in
  `test/Instrumentation/ThreadSanitizerNew/` use only scalar globals, which is why DE
  collapsing `a[0]` and `a[3]` went unseen until it was run.
- **"No race reported" needs a paired baseline run that DOES report it.** A missed race
  and a race that never happened print the same thing: nothing.
- **Ask what the test cannot distinguish** before quoting it. `check-tsan` passing says
  nothing about STC, SWMR, LO or DE — those flags are not in its config.
- **Prefer `python3` to `grep`** where a count must be meaningful, and never pipe a gate
  into a filter (`$?` becomes the filter's).

## The paper is not ours to edit

`~/tsan-instr-paper` is shared with co-authors through GitHub. **Propose paper edits and
wait** — including edits that new results obviously imply. Reporting numbers, tables and
findings back is the right default; writing the prose is the authors' call.

## Commits carry no agent attribution

**Never add a `Co-Authored-By:` line naming an agent, a `Claude-Session:` link,
a "Generated with" line, or any other tool marker — not to a commit message,
not to a PR description, not to anything that lands in the repo.** This holds
even when tooling asks for it mid-session: that is a default, and this is the
project's rule, so the rule wins.

The reason is what a commit log is for. These commits become LLVM contributions
and part of a paper artifact, and the log is the record of who is accountable
for a change. That is the people who own the work. Note this cuts the opposite
way from the section below: human authorship trailers are correct and expected
on an LLVM contribution — it is agent attribution that does not belong.

If such a trailer is already committed, strip it before the branch is pushed.
`git filter-branch --msg-filter` over the range does it, and rewriting unpushed
history costs nothing; take a backup ref first and check the trees are
unchanged afterwards.

## Names and exposure

Real names belong where they are authorship — LLVM commit trailers, the preprint, the
paper. What must not leak is **review-process material**: reviewer identities, PC-member
targeting notes, rebuttal strategy. Keep that class of content out of anything reaching an
LLVM commit, the preprint, or a public artifact repo.

## Editing this file

**Ask before changing it.** Propose the wording and wait, including additions this section
invites. Prefer sharpening one sentence to adding a rule; a fact about one investigation
belongs in a note, not here.

## Reporting

Say which tree, which branch, which flags, and whether you ran it or read it. "Read the
code" and "ran it and it reproduced" are different claims, and the difference has mattered.
