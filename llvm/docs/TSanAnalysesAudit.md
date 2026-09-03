# ThreadSanitizer static analyses — audit ledger

One row per function of the five analyses, the instrumentation pass and the
runtime additions; one section per analysis comparing what the paper proves
with what the code does. Line numbers are those of `b4bf8b8f4613` (the
rebuttal tree); fixes reference commits on `tsan-audit`.

**Verdict legend.** `sound` — fail-closed as written, argued in its row.
`fixed:<id>` — a fail-open shape was confirmed by running the pass and closed
by the named fix. `pending:<id>` — confirmed or predicted by reading, fix
scheduled. `precision` — conservative in a way that only costs reach.
`hygiene` — no soundness bearing. `decision:<who>` — a policy choice recorded
in the plan.

**Method.** Three inventories (every function, its contract, its default on an
unknown), sixteen minimal-IR probes against the built `opt` for every
predicted fail-open, then a fix with a negative test that fails on the parent
commit and a positive control that still elides. Reach is measured per commit
(`-mllvm -stats` on sqlite3.c, shell.c, memcached's 26 modules).

## Confirmed lost-race shapes (all in the shipped default configurations)

| # | shape | root | fix |
|---|---|---|---|
| 1 | object escapes whole, then a field is written (`foo(&s); s.f1 = 1;`) | exact (object, field-path) lookup | EA-2 — fixed d06851ec7a6d |
| 2 | `&x` stored into an already published container | container's escaped *state* never consulted | EA-3 — fixed cbeb570ed61c |
| 3 | `&x` published via a `cmpxchg` value operand | `EscReasonTy` too narrow; reason truncated to 0 | EA-1 — fixed 667343f20eaf |
| 4 | `&x` stored through an unidentifiable pointer | incomplete object walk skipped | EA-5 — fixed 957f8e506697 (refined 67e9532aa926) |
| 5 | pointer loaded from a `memcpy`-filled struct, dereferenced | slot with no recorded pointee answers local | EA-6 — fixed 09b9a74d17b7 |
| 6 | access through a loaded pointer, object published later | sound-FS rule walks the wrong object | EA-9 — fixed fe9c96cfd10f |
| 7 | `helper()` after `pthread_create()` in a non-`main` function | creator's callees never marked MT | STC-1 — fixed a633cc3f0e77 |
| 8 | `extern` global read here, written elsewhere (memcached `current_time`) | no linkage check | SWMR-1 — fixed def2cf34faeb (benchmark-confirmed by tsan-exp, N=10) |
| 9 | callee releases the caller's lock | callee releases unmodelled | LO-2 — fixed 20c178992bfd |
| 10 | any name containing `lock` is an acquisition | substring matching | LO-1 — fixed 12399f969130 |
| 11 | dominance across `write(2)` | `TLI::isSyncFree` defaults true | DE-5 — fixed 450fc39a8545 |
| 12 | post-dominance across `read(2)` | same | DE-5 — fixed 450fc39a8545 |
| 13 | a store reached from a lock-free path counted as protected (held-lock record from an intermediate visit; found during the audit, reproduced on 297881ddc1c5) | worklist seeded with the entry only; empty held set never recorded | LO-3/5 — fixed c8debe12f363 |

## Design versus paper

### STC — single-threaded context (`3a-1-stc.tex`, Prop. stc-conc)

*Paper.* An access executed only before the program creates additional
threads cannot race. MT is computed per function, per block in `main`: blocks
of `main` creating threads; functions creating threads and their transitive
callers; address-taken functions; callees of MT functions; successors of MT
blocks of `main` and their callees. "May call" is an over-approximation.

*Code.* `runSTMTAnalysis` (SingleThreaded.cpp:199-284), `computeMainMTBlocks`
(:102), `propagateMTFromMain` (:122). Deviations: (a) the rule "callees of MT
functions" is not applied to a *creator's* callees — `pending:STC-1`; (b) a
function this unit never calls and never takes the address of defaults to ST,
which is only valid with whole-program visibility — `pending:STC-2` (per-TU:
external linkage ⇒ MT); (c) a call to a bodiless function is not a possible
thread creation — `pending:STC-3`; (d) the paper's "program starts at `main`,
never called" is assumed and unchecked. The dynamic variant (`th:stc-dyn`) is
implemented as the paper says: counter in the runtime, incremented at create,
decremented at join, guard load `monotonic` and never instrumented — `sound`.

### SWMR (`3a-2-swmr.tex`, Prop. swmr)

*Paper.* A read of locations L cannot race if every write to L occurs in ST
context.

*Code.* `findSWMRGlobals` (:314-367) elides *all* accesses to a global no MT
instruction writes — reads and writes; the writes so elided are ST by the
same verdict, so this is a restriction of the proposition, not an extension —
`sound`. Address escape ⇒ not read-only — `sound`. Externally linked globals
qualified — `fixed:SWMR-1`. Inherits STC's verdict for the writes, so STC-1/2/3
also gate it.

### LO — lock ownership (`3a-3-lo.tex`, `appendix/lo-proof.tex`)

*Paper.* Interprocedural, context-insensitive must-held locksets per access;
per location the intersection over MT accesses; an access needs no
instrumentation if every location it may touch is owned, given an
over-approximation of the accesses to each location.

*Code.* Restricted to `GlobalVariable`s with direct users
(`findProtectedGlobalVariables`) — a restriction, `sound` as far as it
goes once the "over-approximation of accesses" premise holds: the variable
must be a local-linkage definition (another unit cannot name it) and every
use of its address the pointer operand of a load or store (nothing reaches
it through a path the scan does not see) — `fixed:LO-4/4b` (12399f969130).
Lock identity by global + constant offset — `sound` (51c96792787b).
Acquisition recognised by name substring ("block" contains "lock") —
`fixed:LO-1` (12399f969130; exact tables, pthread/mtx/omp). Callee
releases — `fixed:LO-2` (20c178992bfd): syntactic release summaries closed
over the call graph, applied at every call before the callee's exit
acquisitions. Opaque calls (indirect, non-transparent declarations) —
policy C as decided: may release any non-private mutex and a private one
only through a callback; private = local-linkage global used only as a
mutex operand; callback candidates = externally visible functions and
address-taken locals other than thread start routines. Cost on memcached:
LO alone 46 → 9 elisions (its mutexes are external globals). Must-analysis
initialisation and the recording of held sets from an intermediate iteration
— `pending:LO-3/5` (predicted; the constructed counterexample did not
reproduce, so hardening only).

### EA — escape analysis (`3a-4-ea.tex`, Props. ea0/ea1)

*Paper.* An object escapes if its pointer is stored to a global, stored into a
field of an escaped object, passed to a callee whose parameter escapes, or
returned; a non-escaped object is thread-local, per object.

*Code.* A flow-sensitive dataflow per block with a points-to relation and
field paths, plus bottom-up/top-down IPA. Extensions beyond the proposition:
per-program-point elision, made sound by the later-escape rule
(b4bf8b8f4613) — `sound` on alloca objects, `fixed:EA-9` for objects reached
through memory; field sensitivity (`{s,[1]}` vs `{s,[2]}`) — `sound` for
programs without out-of-bounds arithmetic. Holes against the definition's own
clauses: whole-object escape not covering a field (`fixed:EA-2`), "stored into
a field of an escaped object" only in one order (`fixed:EA-3`), reason bits
lost (`fixed:EA-1`, `fixed:EA-4`), unknown objects dropped (`fixed:EA-5`),
unlisted opcodes and loaded pointers with no recorded pointee
(`fixed:EA-6`), argument escape for functions reached other than by a direct
call (`fixed:EA-7`), library retention table (`fixed:EA-8`).

### DE — dominance elimination (`3b-redundancy.tex`, `appendix/de-proofs.tex`)

*Paper.* Redundant if dominated by an access to the same (must-alias)
location, write⇒write, with all interprocedural paths free of release-like
instructions (unlock, create) and external calls; post-dominance additionally
requires no acquire-like instructions, no loops and no calls on the paths.

*Code.* `eliminateInstrByPrePostDominance` (ThreadSanitizer.cpp:1720-1862)
with `locationCovers` (must-alias **and** size coverage — stronger than the
paper's footnote), `classifySyncEffect`, `scanPaths`. Extensions: calls that
are `nosync`/sync-free-by-summary are allowed on dominance paths (the paper
forbids external calls; a call proven sync-free is not one — `sound`);
post-dominance allows calls proven to return — option (ii) as decided:
willreturn on call or callee, or defined and loop-free, for every call
including libfuncs and intrinsics — `fixed:DE-3` (3b23aa757035; yield
unchanged at -O2 where the attributes are inferred); fd I/O (`read/write/pread/pwrite/open`)
fell to the sync-free default although TSan's interceptors make them
release/acquire edges — `fixed:DE-5` (450fc39a8545, listed as not
sync-free; inverting the default is `decision:Alexey`, with a dominance
yield cost). Lock names were never a DE problem: `classifySyncEffect`
always used TargetLibraryInfo's exact table; `invoke` callees not scanned — `fixed:DE-1`; the sync-free summary
consulted its own not-yet-built static pointer — `fixed:DE-2`
(2aa46b60cb86: a module analysis, explicit pointer; measured reach at -O2
identical because FunctionAttrs already infers nosync/willreturn — the
summary only adds reach on unoptimised IR); transitive cover chains —
`sound` (argued) and re-scanned against the surviving root — `fixed:DE-4`
(dc7d9e0a8a20).

*Model deviation, quantified (tsan-exp, `tools/eviction-stress`, f80e80b1dbe6).*
The paper's argument that a covered access is redundant assumes the covering
access's shadow record is still there when the conflicting access arrives.
TSan's shadow is four slots per granule; stock re-inserts the record at every
access, an elided access does not. A synthetic program that evicts the
dominating store's record between the two stores (fillers, trace-position
victim, no happens-before) shows it: stock and sound-only report the race
1000/1000 (the second store re-inserts), DE-only and AllOpt+peel 748/1000 —
0/252 when the record had been evicted, 748/748 when it had not. On the
real workloads (SQLite wal-index, memcached `current_time`) eviction was
measured equal across builds and no race was lost to it, but the mechanism
is real and belongs in the paper's statement of the model (bounded shadow),
not in the soundness claim for unbounded shadow. `decision:Alexey` whether
dominance elimination should be qualified this way in the revision or
guarded (e.g. keep a covered access when the path between crosses many other
granules); no code change made.

The symmetric variant (both effects on one granule) shows the deviation is
two-sided: where the dominating record was evicted, DE loses the A–B race in
0/236 of those runs and keeps C–B in 236/236 of them, while stock's
re-inserting store evicts a third party's record and loses C–B in 71/71 of
the runs where that happens; overall stock 0.91 races per run (A–B 22.8 %,
C–B 68.2 %) against DE 0.93 (17.4 %, 75.3 %) — complementary effects,
comparable totals. Also established (identical in every build): TSan clears
the granule's shadow after a race report, so a second race on the same
granule reported by the same thread is wiped by the first report.

### Whole-program mode via summaries (S)

`-tsan-use-analysis-summaries` with the linked program analysed once under
`-tsan-whole-program` (fafbebedb41e): only externally visible entities are
written, sorted, stamped with `-tsan-summary-id` and ignored on mismatch;
readers overlay on the per-unit analysis (a summary-listed external function
is not an externally reachable root; summary-listed variables join the SWMR
and protected sets; the escape whitelist applies to external callees only);
a seeded compile never overwrites its summary; `-tsan-summary-dir` replaces
the fixed directory; a second header line records the writer's whole-program
setting as provenance (an IR hash is not something a unit that has only
itself could verify; the id is the check). Measured on memcached (26 modules,
-O2): sound-only
per-unit 6356 → with summaries 5852; AllOpt+peel 6865 → 6262 (the unsound
per-unit `-tsan-whole-program` switch gives 5975 for comparison). The
premise the mode asserts: nothing outside the linked module calls into it
except through addresses it takes itself, and `main`.

### Preservation at the application level (tsan-exp, N=10, both stock baselines)

On 297881ddc1c5 (instrumentation identical to the final tree for every
per-unit configuration): SQLite sound and AllOpt+peel report exactly the
stock set (5 L1 / 2 L2); memcached sound-only loses nothing at L2; memcached
AllOpt+peel shows two L2 rows absent — conn_new@memcached.c:761 and
lru_pull_tail@items.c:1207, both readers of `current_time`. Verified from the
IR of both builds: every access to `current_time` is instrumented identically
(clock_handler's two writes, every reader, the two reported sites included;
peeling adds reads), and the only elided read of it anywhere is a covered
read inside do_item_update, neither site. The same pair is 0/10 under the
old stock and 0/30 on def2cf34faeb and 10/10 only under this hash's stock:
it flips with the surrounding instrumentation in both directions. tsan-exp
then confirmed the relocation: under AllOpt+peel clock_handler's write is
reported 10/10 paired with do_item_link:495, lru_maintainer_thread:1671 and
try_read_command_ascii:493 (as under stock) and 3/10 with
lru_maintainer_juggle:1426 — only the two pairs moved. Detection of a race
whose covered read is elided moves from the reader side to the writer side,
which names whichever reader's record is in the cell: the expected
consequence of dominance elimination. Under the L3 key (kind + location +
writer site, readers collapsed) the race is preserved; instrumented accesses
to `current_time` on the binary: stock 71, sound 70, AllOpt+peel 74. `evict_watch` on
`&current_time` (N=5, ASLR off, memtier workload): 41.6M / 39.0M / 38.6M
evictions per run on that granule for stock / sound / AllOpt+peel, all
uncovered and all plain in every build, within 7 %; the writer's record
survives equally, timing decides which reader is named.

Final rows (43111f84d936 summaries builds and the ad0623610ef6 per-unit
builds, N=10 against the 297881ddc1c5 stock): memcached sound + whole-program
summaries keeps all ten frequent stock sites at 10/10, conn_new:761 included,
and loses nothing at L3 outside the 1/10 conn_new burst family; AllOpt+peel +
summaries the same except conn_new:761 (the DE relocation above). Per-unit
ad0623610ef6: sound keeps the ten frequent sites at 9–10/10, AllOpt+peel nine
of ten; every L3 loss is the burst family. Whole-binary static counts with
summaries: sound 6197 → 5717, AllOpt+peel 6658 → 6107.

### Bounded shadow and the optimised builds (P3/P4, tsan-exp with `evict_watch`)

SQLite threadtest3 on 43111f84d936, N=10, ASLR off, the three wal-index
header granules watched (medians, total / uncovered / uncovered-plain):
stock 39008/2220/612, 653104/70282/0, 224874/24346/0; tsan-sound
38462/2181/584, 656228/68186/0, 225423/24561/0; AllOpt+peel
38080/2206/590, 642671/68868/0, 220406/25043/0 — every counter within 3 %
across builds, the ~590 uncovered plain records per run all on the
nBackfill/aReadMark[0] word, none on the read-mark words. The 2.3× seen in
single traced runs was the tracing overhead. On the racing granules the
optimised builds neither increase nor decrease eviction: the bounded-shadow
effect on these races is a property of the workload and the runtime, not of
the instrumentation.

## Function ledger

Each row's last cell carries the measured coverage of that function (`lines`, `branches`) from the run described under *Coverage*; rows for several functions carry the first one's.


Columns: function · contract · what it must preserve · verdict · tests.
Lines as of b4bf8b8f4613.

### EscapeAnalysis.cpp / .h

| function | contract | preserves | verdict | tests |
|---|---|---|---|---|
| `printEscReason` (63) | debug print of reason bits | — | hygiene (indexed bit 6 of a 6-bit set; widened by EA-1) | — · lines 0.00%, branches 0.00% |
| `printIPAFuncEscInfo` (75), `printSCC` (99), `dbg*` (108-140), `printArgEscStatus` (142) | debug | — | hygiene | — · lines 0.00%, branches 0.00% |
| `isLocalAndExactFunc` (160) | defined, exact-definition function | indirect-call guard (`nullptr` ⇒ false) | sound | ipa-* · lines 100.00%, branches 100.00% |
| `isPointerArgument` (164) | pointer-typed `Argument` | — | sound | ptr-args.ll · lines 100.00%, branches 100.00% |
| `EscapeState::checkAndUpdEscStatus` (175) | escape a pointee when its pointer is escaped | definition clause 2 | fixed:EA-3 (only external status consulted) | escape-transitive-store-both-orders |
| `EscapeState::forEachPointeeDo` (196) | iterate a slot's pointees | — | sound | aliases.ll |
| `EscapeState::addPointsTo` (207) | record pointer→pointee, transitively | points-to closure | sound (recursion bounded by pair dedup) | aliases.ll |
| `PointsToRelTy::*` (250-340) | two-level map obj→path→pointees; `getPointees` subsumes empty path | path lattice | sound; EA-2 makes the escape side match | field-sensitive.ll |
| `EscapeState::getEscReason` (346) | reason for (obj,path) | lookup must cover prefixes | fixed:EA-2 (exact match) | escape-field-lattice |
| `EscapeState::addEscObjOrReason` (360) | leaf insert, empty-path erases fields | — | sound after EA-2 | — |
| `EscapeState::addEscObj` (405) | escape object and its transitive pointees | definition clause 2 | sound | escape-transitive-store-both-orders |
| `mergeEscapedObjects` (429) | union of escaped sets | meet | sound (normalised by EA-2's lookup) | loops-and-if.ll · lines 100.00%, branches 100.00% |
| `getArgEscBottomTopIPA` (444) | callee's argument escape from its summary | clause 3 | precision: missing function ⇒ escaped; missing arg index inserts 0 — fixed with EA-6/7 (data-operand vs argument index) | arg-esc.ll · lines 87.50%, branches 50.00% |
| `getArgEscTopDownIPA` (455) | caller-side argument escape | clause 3 | sound (missing ⇒ escaped) | arg-esc.ll · lines 87.50%, branches 75.00% |
| `isNonConstGV` (466) | non-constant global | — | sound | — · lines 100.00%, branches 100.00% |
| `getExtObjStatus` (472) / `getExtObjStatusIPA` (565) | syntactic status: global / argument / pointer-returning call | — | fixed:EA-6 (`CallInst` only; invoke result "fresh") | escape-opcode-table · lines 100.00%, branches 87.50% |
| `getIPAFuncRetEscStatus` (488) | callee returns escaped memory? | clause 4 | pending:EA-6 (absent ⇒ false; made true) | ipa-return.ll · lines 100.00%, branches 50.00% |
| `isCallNotReturnEscaped` | library call returns fresh memory? | — | fixed:EA-8 (bfaa2b859100): by prototype, `TLI.has`, no name list; realloc escaping | tli-retention · lines 100.00%, branches 83.33% |
| `isSafeExternalCall` | summary whitelist | — | fixed:EA-8 (bfaa2b859100): no summary ⇒ nothing safe; empty line ⇒ nothing known | — · lines 90.91%, branches 83.33% |
| `isCallMayEscape` (536) | may the call's result be non-fresh memory | — | sound for indirect/external; inherits `getIPAFuncRetEscStatus` | ipa-passing-func-ptr.ll · lines 94.44%, branches 75.00% |
| `EscapeAnalysisInfo` ctor (588) | RPO worklist fixpoint over blocks | monotone transfer, finite lattice | sound; only reachable blocks seeded (see `findObjInBBEscState`) | loops-and-if.ll · lines 100.00%, branches 43.75% |
| `updRetEscStatus` (646) | mark return-escape from the in-progress state | clause 4 | sound (compensates for stale committed state) | ipa-return.ll · lines 100.00%, branches 62.50% |
| `addEscapedPtrArgs` (672) | seed entry state with escaped pointer args | clause 3 | sound | func-arguments.ll · lines 100.00%, branches 70.00% |
| `compBBEscapeState` (690) | transfer function | all clauses | fixed:EA-5 (incomplete operand skipped); duplicated add at :729/:743 hygiene | escape-incomplete-walk · lines 100.00%, branches 0.00% |
| `mergePredEscapeStates` (761) | meet over predecessors | — | sound (missing pred = empty) | loops-and-if.ll · lines 100.00%, branches 100.00% |
| `typeContainsPointerType` (779) | aggregate transitively holds a pointer | memcpy modelling | sound (9f9eab6db1e0) | escape-adversarial · lines 100.00%, branches 100.00% |
| `getEscInfoCall` (793) | classify a call operand use | clause 3 | sound for indirect/inline-asm/varargs; memcpy source must be an alloca (precision); aggregate-typed args — fixed:EA-6 | ipa-*, escape-adversarial · lines 96.43%, branches 84.85% |
| `getEscInfoLoad` (897) | volatile load = escape | — | sound | — · lines 100.00%, branches 100.00% |
| `getEscInfoStore` (906) | stored pointer aliases the destination | clause 1/2 | fixed:EA-5 (empty destination ⇒ no escape); non-pointer value — fixed:EA-6 | escape-incomplete-walk, escape-opcode-table · lines 92.86%, branches 80.00% |
| `getEscInfoAtomicRMW` (929) | value operand escapes | — | sound | — · lines 100.00%, branches 62.50% |
| `getEscInfoAtomicCmpXchg` (944) | value operands escape | — | fixed:EA-1 (reason truncated) | escape-cmpxchg-publishes · lines 100.00%, branches 60.00% |
| `getEscInfoGetElementPtr` (959) | vector GEP escapes | — | fixed:EA-1 | — · lines 80.00%, branches 50.00% |
| `getEscInfoICmp` (971) | comparison is not an escape | — | sound (a compared pointer cannot be dereferenced elsewhere) | — · lines 93.75%, branches 70.00% |
| `getEscInfoRet` (1001) | returned pointer escapes | clause 4 | fixed:EA-6 (aggregates) | escape-opcode-table · lines 100.00%, branches 100.00% |
| `getEscInfoForOpnd` (1012) | dispatcher | all | fixed:EA-6 (default NO_ESCAPE) | escape-opcode-table · lines 87.76%, branches 80.36% |
| `isDereferenceableOrNull` (1046) | CaptureTracking helper | — | hygiene (vestigial) | — · lines 100.00%, branches 0.00% |
| `findObjInBBEscState` (1062) | block's escaped set lookup | — | hygiene→pending: bare `assert` on an unreachable block (release UB); guard with "not analysed ⇒ escaped" | — · lines 100.00%, branches 0.00% |
| `findObjInFuncEscState` (1070) | per-function union (flag) | — | sound (cache invalidated, 9864b0e9e094) | escape-flow-insensitive · lines 93.33%, branches 87.50% |
| `isEscapedForBBImpl` (1088) | external status, then block/union state | — | sound | simple.ll · lines 100.00%, branches 94.44% |
| `isEscapedForBB/IPA` (1115/1122), `isEscapedInFuncIPA` (1129) | wrappers | — | sound | — · lines 0.00%, branches 0.00% |
| `forEachPointeeDo` (1149) | per-block pointees | — | same `assert` note as above | — · lines 100.00%, branches 62.50% |
| `getFullEscReasonForBB` (1158) | OR of external and state reasons | — | sound | — · lines 100.00%, branches 0.00% |
| `isEscapedForFunc` (1165) | union over blocks, OR-ed | summary input | sound | ipa-top-bottom.ll · lines 100.00%, branches 87.50% |
| `printEscapingForBB` (1181), `print` (1204) | printers | — | hygiene | all Analysis/EscapeAnalysis tests · lines 100.00%, branches 85.71% |
| `EscapeAnalysis::run` (1213), printer (1219) | non-IPA mode | — | sound; note PASS-1 double-sanitize did not reproduce | — |
| `setAllPtrArgsNotEscaped` (1231), `isRecursiveCallGraphNode` (1238) | SCC optimistic init | fixpoint | sound with `traverseCGBottomTop`'s iteration | ipa-recursive.ll, ipa-SCC2.ll · lines 100.00%, branches 100.00% |
| `updIPAFuncEscInfo` (1249) | write bottom-up summary | — | sound | ipa-simple.ll · lines 100.00%, branches 45.45% |
| `traverseSCCsAndInitIPAEscInfo` (1276) | find recursive functions/SCCs | — | sound; `assert` on null node hygiene | ipa-SCC2.ll · lines 100.00%, branches 78.57% |
| `getFuncToCallSitesMap` | direct call sites per function | clause 3 top-down | sound as a call-site map; the address-taken rule lives in evalTopDownArgEscStatus | ipa-address-taken-table (planned) · lines 100.00%, branches 91.67% |
| `EscapeAnalysisInfo::TLI` (member) | library knowledge for the function | — | fixed (c402aec8dec3): was a reference to a function-analysis result freed by `PreservedAnalyses::none()` while the module result lived on — a use-after-free on every later query, latent until prototype validation dereferenced it | tli-retention (crashed before) · lines 100.00%, branches 43.75% |
| `evalTopDownArgEscStatus` | caller-side argument escape | clause 3 | fixed:EA-7 (bfaa2b859100): address taken or externally visible ⇒ every pointer argument escapes | ipa-address-taken-table (ed) · lines 100.00%, branches 16.67% |
| `traverseCGBottomTop` (1419) | leaf-first summaries | — | sound; declarations get no entry (⇒ escaped) | ipa-top-bottom.ll · lines 100.00%, branches 75.00% |
| `isFuncPassedToObjCSelector` (1472) | ObjC selector mitigation | — | precision (substring) | — · lines 38.89%, branches 22.22% |
| `traverseCGTopDown` (1494) | callers-first refinement | — | sound (non-local linkage skipped ⇒ escaped) | ipa-top-bottom.ll · lines 97.56%, branches 82.14% |
| `readNonEscapingFuncs` (1555), `writeIPASummary` (1788), `getFileNameFromPath` (1605), `createLogDir` (1611) | summary files | whole-program mode | pending:S (trust, keying, determinism) | (planned) · lines 82.35%, branches 59.09% |
| `EscapeAnalysisGlobalInfo` ctor (1619) | driver | — | sound | ipa-* · lines 100.00%, branches 29.17% |
| `isEscapedUndrlObjOrPointee` (1683) | TSan entry: object or pointee escaped at block | — | sound (incomplete ⇒ escaped; escape-unidentified.ll) | escape-unidentified · lines 100.00%, branches 57.14% |
| `isEscapedForBBTSan` (1713) | per object, then pointees | — | fixed:EA-4 (reason clobbered); loaded slot with no pointee — fixed:EA-6 | escape-loaded-from-escaped-slot, escape-loaded-unknown-pointee · lines 100.00%, branches 0.00% |
| `isEscapedUndrlObjOrPointeeAnywhere` (1740) | per-object variant | sound-FS rule | fixed:EA-4 | escape-later-through-loaded-pointer · lines 100.00%, branches 0.00% |
| `isStructFieldGEP` (1867), `getUnderlyingObjectWithPath` (1903) | object + field path | field lattice | sound (non-struct GEP resets to whole) | field-sensitive.ll · lines 76.47%, branches 80.00% |
| `getUnderlObjThroughLoads` (1970) | look through loads (unbounded) | — | precision; bound with MaxLookup (hygiene) | — · lines 100.00%, branches 100.00% |
| `getUnderlObjsWithoutPHIInvCheck` (1986) | phi/select fan-out | — | fixed:EA-2 (path reset to empty) | escape-field-lattice · lines 100.00%, branches 100.00% |
| `getUnderlyingObjectFromInt` (2026) | integer→object | — | sound (ValueTracking copy) | escape-incomplete-walk · lines 100.00%, branches 64.29% |
| `getUnderlObjsForCodeGenWithoutPHIInvCheck` (2054) | fail-closed object walk | — | sound | escape-unidentified · lines 97.73%, branches 77.27% |
| `getUnderlyingMayEscObjs` (2103) | public entry with `IsComplete` | — | sound; callers must honour `IsComplete` (EA-5) | — · lines 100.00%, branches 22.22% |
| `EscapeAnalysisGlobal::run` (1849), printer (1854) | plumbing | — | sound | — |

### TargetLibraryInfo.cpp (fork additions)

| function | contract | verdict | tests |
|---|---|---|---|
| `doesArgEscape` (1417) | does libc retain the argument | fixed:EA-8 (bfaa2b859100: `setbuf/setvbuf` retain arg 1, realloc arg 0 aliases the result); default true sound | tli-retention |
| `isReturnValueEscaping` (1723) | does libc return non-fresh memory | fixed:EA-8 (bfaa2b859100: `realloc` family escaping; section III left as is, unreachable for pointer returns) | tli-retention |
| `isSyncFree` (1836) | libc function synchronises? | fixed:DE-5 (450fc39a8545: fd I/O not sync-free; default still true — decision:Alexey) | elim-by-dominance-across-fd-io |
| `isLockAcquireFunction` / `isLockReleaseFunction` (2123/2148) | exact-name lock tables | sound; LO now has the same exact-name discipline (LO-1) | elim-by-dominance-across-acq-rel-* |

### SingleThreaded.cpp / .h

| function | contract | preserves | verdict | tests |
|---|---|---|---|---|
| `collectAccessingInstrs` (45) | users of a global through ConstantExpr/alias | over-approximation of accesses | sound | aggregate-and-st-only.ll · lines 90.00%, branches 60.00% |
| `isKnownThreadCreator` (58) | creator by exact name / mangled prefix | rule 2 | sound; single list after NAMES-1 | thread-creators.ll · lines 100.00%, branches 100.00% |
| `isSingleThreaded(Function*)` (67) | wholesale ST; `main` never | — | sound at the query | single-threaded.ll · lines 100.00%, branches 87.50% |
| `mayCreateThread` (75) | call may start a thread | rule 2 | fixed:STC-3 (bodiless callee ⇒ false) | single-threaded-unknown-external · lines 100.00%, branches 100.00% |
| `blockCreatesThreads` (97), `computeMainMTBlocks` (102) | MT blocks of `main` = successors of creating blocks, closed | rule 1/5 | sound | single-threaded.ll · lines 100.00%, branches 0.00% |
| `propagateMTFromMain` (122) | callees of MT instructions of `main` | rule 5 | sound | single-threaded.ll · lines 100.00%, branches 83.33% |
| `isSingleThreaded(Instruction*)` (143) | per-instruction in `main` | rule 1 | sound | preservation_stc_main.cpp · lines 100.00%, branches 87.50% |
| `identifyBaseThreadCreators` (165) | seed creators | rule 2 | sound | thread-creators.ll · lines 100.00%, branches 62.50% |
| `markFuncAndAllCalleesAsMultithreaded` (175) | mark F and callees MT | rule 4 | **fixed:STC-1** (no-op at its call site) | single-threaded-creator-callee · lines 100.00%, branches 75.00% |
| `runSTMTAnalysis` (199) | fixpoint over the call graph | rules 1-5 | fixed:STC-2 (default ST for unseen functions) | single-threaded-linkage · lines 100.00%, branches 53.95% |
| ctor (286) | driver; ignores `runSTMTAnalysis`'s return | — | hygiene | — |
| `findSWMRGlobals` (314) | globals never written in MT | Prop. swmr | **fixed:SWMR-1** | swmr.ll, swmr-linkage · lines 100.00%, branches 62.50% |
| `print` (369), `createLogDir` (416), `writeSummary` (424), `readSummary` (451) | printer, summaries | whole-program mode | pending:S | (planned) · lines 100.00%, branches 100.00% |
| `SingleThreaded::run` (502), printer (516) | plumbing | — | sound | — |

### LockOwnership.cpp / .h

| function | contract | preserves | verdict | tests |
|---|---|---|---|---|
| `getTopDownSCCList` (30) | unused | — | hygiene (dead) | — · lines 0.00%, branches 0.00% |
| `intersectLockStates`, `computeMeet` | must-meet over predecessors | must-lockset | fixed:LO-3 (c8debe12f363): RPO seeding, so a skipped predecessor is a back edge only (optimistic seed of the standard fixpoint) | loop-unlock-in-body, stale-held-record · lines 100.00%, branches 40.91% |
| `canonicalLockIdentity` (132) | global + constant offset, else unknown | lock identity | sound (51c96792787b, 35a2bae9aad5) | lock-identity.ll · lines 93.75%, branches 83.33% |
| `getLockCallInfo` | classify a call as lock/unlock | — | fixed:LO-1 (exact tables); defined wrappers go through summaries | exact-names · lines 94.44%, branches 94.44% |
| `handleLock` (181), `handleUnlock` (200) | state transitions | — | sound (no assert; pair only with an acquisition) | lock-identity.ll · lines 83.33%, branches 22.73% |
| `applyTransferFunc` | transfer incl. callee releases, exit acquisitions, opaque calls | interprocedural must-lockset | fixed:LO-2 (20c178992bfd); unknown release clears — sound | callee-releases, external-call-policy · lines 96.43%, branches 70.00% |
| `computeReleaseSummaries`, `mayBeCalledBack`, `isPrivateMutex`, `applyOpaqueCall`, `applyCalleeReleases` | syntactic may-release facts, callback set, private-mutex policy C | interprocedural must-lockset | fixed:LO-2 — sound: releases are a may-set applied as definite; a thread start routine cannot release the creator's mutex | external-call-policy · lines 98.86%, branches 83.33% |
| `buildSummary` | per-function fixpoint + exit state | — | fixed:LO-3/5 (c8debe12f363): every block seeded once in RPO; the held-lock record is replaced on every visit and erased when empty | stale-held-record · lines 100.00%, branches 66.67% |
| `doIPALockOwnershipAnalysis` (378) | bottom-up SCC iteration | — | sound | — · lines 95.45%, branches 85.71% |
| `loadSummary`, `writeSummary` | whole-program summaries: external entities only, sorted, id-stamped; overlay on the per-unit result | whole-program mode | fixed:S (fafbebedb41e) | summary-whole-program.ll · lines 100.00%, branches 75.00% |
| `moduleIgnoresSync` (456) | give up under ignore-sync annotations | — | sound | ignore-sync.ll · lines 100.00%, branches 75.00% |
| ctor (469) | driver | — | sound | — |
| `isAnnotationFunc`, `isTryLockFunc`, `isSharedLockFunc` | name predicates | — | sound: operate only on the exact-name sets (LO-1) | annotations.ll, locks.ll · lines 100.00%, branches 75.00% |
| `findLockUnlockFunctions` | recognise lock/unlock functions | — | fixed:LO-1 (12399f969130; exact names) | exact-names · lines 100.00%, branches 78.57% |
| `getLocksProtecting` (592) | held set at an instruction | — | sound | — · lines 100.00%, branches 50.00% |
| `findProtectedGlobalVariables`, `isDirectAccessOf` | intersection over MT accesses of a private, unescaped global | Prop. protection | fixed:LO-4/4b (12399f969130) | linkage-and-escape · lines 100.00%, branches 58.57% |
| `print` (676), `LockOwnership::run` (781), printer (794) | plumbing | — | hygiene (dead commented body) | — · lines 100.00%, branches 100.00% |

### ThreadSanitizer.cpp / .h

| function | contract | preserves | verdict | tests |
|---|---|---|---|---|
| ctor (345) | flag sanity | — | hygiene (`llvm_shutdown` on conflict is not an exit) | — |
| `~ThreadSanitizer` (362) | disabled stats dump | — | hygiene (dead) | — |
| `tryPeelLoops` (628) | peel first iteration when a loop-invariant access exists | — | sound (transformation only) | peeling.ll · lines 100.00%, branches 100.00% |
| `ThreadSanitizerPass::run` (702) | per-function driver | — | pending:PASS-2 (`optional<T*>` null); PASS-1 not reproduced | — |
| `ModuleThreadSanitizerPass::run` (757) | module driver; `SFI` static | — | pending:DE-2 | elim-by-dominance-inter-calls |
| `initialize` (843) | declare callees | — | sound | — |
| `isVtableAccess` (997), `shouldInstrumentReadWriteFromAddress` (1005), `addrPointsToConstantData` (1031) | upstream elisions | — | sound (upstream) | read_from_global.ll, tsan_address_space_attr.ll · lines 100.00%, branches 100.00% |
| `updateEscapeStatistics` (1054) | counters | — | hygiene (OTHER/INVALID never counted before EA-1) | — · lines 94.44%, branches 93.75% |
| `isThreadCreatorName` (1085) | second creator list | — | fixed:NAMES-1 pending | — |
| `allLoadsAcquire` (1095) | every load of G is acquire | release-like publication | fixed:EA-9 pending (no linkage check) | escape-later-through-loaded-pointer · lines 92.86%, branches 75.00% |
| `LaterEscapes::allReleaseLike` (1136) | nothing non-release-like reachable | — | sound given a correct `Seen` | escape-adversarial |
| `collectLaterEscapes` (1146) | reachable escape sites of the access's object | sound-FS rule | **fixed:EA-9 pending** (wrong object; memcpy source) | escape-later-through-loaded-pointer · lines 100.00%, branches 75.00% |
| `attributeFlowSensitiveElision` (1236) | statistics | — | hygiene | — · lines 41.67%, branches 41.67% |
| `chooseInstructionsToInstrument` (1273) | the elision pipeline | all propositions | sound in order; each step's analysis verdict is what this ledger audits; LO/STC/SWMR steps gain counters (STATS-1) | ThreadSanitizerNew/* |
| `createInstrIndexMap` (1458), `getReverseReachable` (1467) | indices; predecessor closure (memoised) | — | sound | elim-by-dominance-paths |
| `locationCovers` (1494) | must-alias and size coverage | condition 2 | sound (stronger than the paper) | elim-by-dominance-locations |
| `isTsanAtomic` (1527) | atomic access | — | sound | — · lines 100.00%, branches 100.00% |
| `classifySyncEffect` (1537) | acquire/release/unknown/may-not-return per instruction | conditions 4/5 | fixed:DE-5 (450fc39a8545); fixed:DE-3 (3b23aa757035, termination bit for every call); fixed:DE-2 (2aa46b60cb86, explicit SFI pointer) | elim-by-dominance-across-fd-io, across-acq-rel-*, across-intrinsics |
| `isInstrDangerous` (1604) | any sync effect | — | sound | — |
| `scanPaths` (1619) | union of effects on all paths, incl. the removed access's cycle | conditions 4/5 | sound; DE-4 root re-check added (dc7d9e0a8a20) | elim-by-dominance-paths, elim-by-dominance-chain-across-latch |
| `eliminateInstrByPrePostDominance` (1720) | find covers, chain them | Props. really-red / post | sound (chains argued and, since dc7d9e0a8a20, re-scanned against the root); dead `ToRemove` test hygiene | elim-by-*-simple/diamond/coverage |
| `InsertRuntimeIgnores` (1864) | upstream | — | sound | sanitize-thread-no-checking.ll |
| `sanitizeFunction` (1874) | per-function pipeline | — | sound; `__tsan_disable/enable` regions untested (coverage) | — |
| `checkActiveThreadCount` (2117) | `monotonic` load, `> 1` | th:stc-dyn | sound | (planned: dynstc.ll) |
| `instrumentLoadOrStore` (2139) | emit callback, optional guard | — | sound; vtable paths skip the guard (precision) | tsan_basic.ll |
| `createOrdering` (2242) | upstream | — | sound | atomic.ll · lines 85.71%, branches 87.50% |
| `disableInterceptorForInstr` (2259) | toggle TLS flag around a call | — | fixed:RT-1 (223406c284b6): a `CallInst` can unwind through the frame; guarded only when nounwind | intercepted-call-may-unwind |
| `isPointerEscaped` | interceptor may be skipped only under the sound flow-sensitive rule (not escaped here, later escapes release-like) | Prop. ea0 + sound-FS | fixed:PASS-3 (4df6138b85ca) | memintrinsic-later-escape · lines 100.00%, branches 83.33% |
| `instrumentInterceptedCalls`, `instrumentMemIntrinsic` | skip interceptor for local operands | — | fixed:PASS-3 (4df6138b85ca): toggles report their IR; `struct.timeval` workaround deleted — `signal_thread_sigctx_race.cpp` passes 9/9 across stock/EA/AllOpt+peel without it, the failure was the lost interception itself (hypothesis H1), not signal timing (H2 not needed) | memintrinsic-later-escape, intercepted-call-may-unwind |
| `instrumentAtomic` (2397), `getMemoryAccessFuncIndex` (2485) | upstream | — | sound | atomic.ll |
| `SyncFreeInfo` ctor (2520), `findFunsContainsLoops` (2623) | per-SCC sync-free / loop-free summaries | conditions 4/5 | fixed:DE-1/DE-2 (2aa46b60cb86): `CallBase` scans, pessimistic init, `SyncFreeAnalysis` module analysis, ctor passes itself (sound in bottom-up SCC order) | elim-by-dominance-inter-calls, elim-by-dominance-transitive-clean, elim-by-dominance-invoke-callee · lines 100.00%, branches 0.00% |
| `SyncFreeInfo::isSyncFree` / `isLoopFree` (.h 68/82) | queries, unknown ⇒ false | — | sound | — · lines 100.00%, branches 0.00% |

### Runtime (compiler-rt/lib/tsan, sanitizer_common)

| site | contract | verdict |
|---|---|---|
| `__tsan_active_thread_count` (tsan_rtl.cpp:35; `ThreadCreate` :117 `+1`, `ThreadJoin` :320 `-1`, both `seq_cst`) | counter for the dynamic guard | sound (decrement at join, not exit; detach/fork over-count) |
| `InterceptorEnabled` (sanitizer_common.cpp:23, THREADLOCAL) and its six readers | skip a string/memory interceptor for local operands | sound (real call still performed); RT-1 note |
| `MutexLock/ReadLock` `process_lock` guards (tsan_rtl_mutex.cpp) | ReX bookkeeping only with `g_filter` | sound (1af068d46cae) |
| ReX filter (`tsan_filter.*`, `filter_ctx`) | research code | fixed:RT-2 (0d63553b3e43): behind `COMPILER_RT_TSAN_REX_FILTER` (default OFF); with OFF, upstream's `tsan_rtl_mutex.cpp` compiled with this tree's headers is byte-identical except `__LINE__` immediates, `ThreadState` has upstream's members, `tsan_rtl_access.cpp` differs by `NoteEviction` and its two call sites only; the dead RUN-less `ReXFilter/` tests are deleted (previously: compiled in, hooks dead, `enable_filter=1` paid and never filteredcal |
| `__tsan_enable/__tsan_disable` (tsan_interface.inc) | no-op symbols | fixed:RT-2 (the racy `int GV` counter, the `MemoryRangeFreed` VPrintf and the CheckRaces debug prints are gone) |
| `NoteEviction` (tsan_rtl_access.cpp, eviction branch of both `CheckRaces`), `evict_total` / `evict_concurrent_foreign` / `evict_concurrent_foreign_plain`, flags `print_evictions`, `trace_evictions`, `evict_watch` (per-granule counters, 248f45c260ef) | measure the bounded-shadow loss channel: overwrites of another thread's uncovered access | P3 implemented (5eca63015f63); default path unchanged except one call on the slow branch; the uncovered evictions in lock-heavy code are the interceptors' atomic reads on the mutex word, hence the plain subset | shadow_evictions.c |

## Verification on the final tree

`check-llvm` 31541 passed, 2 failed: `Bindings/OCaml/debuginfo.ml` ("Unbound
module Llvm_debuginfo", the OCaml debuginfo binding is not built in this
configuration; no fork change under `llvm/bindings`) and
`function-pass-alone.ll`, run before the PASS-1 code it tests was built
(passes since). `check-clang` 33721 passed, 0 failed. `check-tsan` 371/0,
`check-asan` 545/0, `check-lsan` 160/0, `check-ubsan` 658/0,
`check-sanitizer` 1332/1 (a unit-test shard of allocator tests, passing 3/3
standalone). The 12-configuration TSan suite matrix 292/0 in every
configuration. The faithful K=5 report replay against stock (294 tests, 137
reporting) on 9ef1fce44e2d: L1-identical in every one of the twelve
configurations; the only "other" buckets are the documented non-deterministic
tests — `race_on_barrier2.c` (either side reports), `fd_location_closed.cpp`
(location descriptor varies) — and `fork_atexit.cpp`, which a K=20
re-replay shows reported in 3/20 stock runs and 4–7/20 under SWMR, DE,
DE+peel and sound-only: a timing-dependent report in every configuration,
stock included, now listed with the other two.

## Coverage

Measured on tsan-audit ad0623610ef6 with the five translation units
(EscapeAnalysis, SingleThreaded, LockOwnership, TargetLibraryInfo,
ThreadSanitizer) rebuilt under `-fprofile-instr-generate -fcoverage-mapping`
into shadow `libLLVMAnalysis.so` / `libLLVMInstrumentation.so` and the IR test
directories (`Instrumentation/ThreadSanitizer{,New}`,
`Analysis/{EscapeAnalysis,SingleThreaded,LockOwnership}`) run against them:

| file | functions | lines | regions | (+ runtime suite, AllOpt+peel) functions | lines | regions |
|---|---|---|---|---|---|---|
| EscapeAnalysis.cpp | 87.8 % | 82.8 % | 75.2 % | 90.8 % | 89.8 % | 82.0 % |
| SingleThreaded.cpp | 82.6 % | 71.6 % | 65.7 % | 96.0 % | 95.3 % | 87.3 % |
| LockOwnership.cpp | 90.0 % | 85.5 % | 75.9 % | 96.7 % | 94.8 % | 84.6 % |
| ThreadSanitizer.cpp | 90.9 % | 81.1 % | 73.3 % | 100 % | 92.0 % | 84.7 % |
| TargetLibraryInfo.cpp (upstream tables included) | 47.6 % | 38.3 % | 26.8 % | 57.1 % | 55.7 % | 55.2 % |

The first three columns are the IR directories alone on ad0623610ef6; the
last three add the 292-test runtime suite under AllOpt+peel on babd3b0a01df
with the audit's tests in place. The only non-debug functions never executed
are dead code: `EscapeAnalysisInfo::isEscapedForBB` (an unused query
variant) and `LockOwnershipInfo::getTopDownSCCList`.

Zero-coverage functions were debug printers, the summary readers/writers
(covered since by the split-file summary tests), and: the dynamic
single-threaded guard, volatile under dominance, atomicrmw publication, the
acquire-only publication rule, post-dominance loop termination, the
two-site lockset meet — each now has a test (44da093b1b45). The runtime
suite (12 configurations) adds coverage on top of this; the fail-closed
branches of every fix in this ledger are executed by the negative test
named in its row.



## Commit map after the history consolidation (2026-09-03)

The hashes cited above are those of the working history, preserved as
`backup/tsan-audit-2026-09-03` (and `backup/tsan-dev-2026-09-03` for the
branch it builds on); the frozen build copies under `/extra/alexey/builds`
carry them in `TSAN_AUDIT_HASH` and in `clang --version`, and a
`CONSOLIDATED_HASH` file beside it. The consolidated history that replaces
it has eight steps over `tsan-dev` (whose own unpushed part is six review
steps on top of three of Alexey's commits):

| frozen copy (old hash) | consolidated step containing its code |
|---|---|
| `b4bf8b8f4613` tsan-dev-b4bf8b8f4613 (rebuttal tree) | `63f45dee92ef` (tsan-dev tip) |
| `def2cf34faeb` tsan-audit-def2cf34faeb (SWMR-1) | `4bb688790f44` |
| `297881ddc1c5` tsan-audit-297881ddc1c5 (twelve shapes) | `4d631b5e4563` |
| `a08292850aee` tsan-audit-a08292850aee (+eviction counters) | `17f24744af64` |
| `ad0623610ef6` tsan-audit-ad0623610ef6 (thirteen shapes, EA-7/8, RT-2) | `17f24744af64` |
| `43111f84d936` tsan-audit-43111f84d936 (summaries mode, evict_watch) | `6a2aed2d00e7` |
| `f80e80b1dbe6` tsan-audit-f80e80b1dbe6 (final code) | `6a2aed2d00e7` |

| fixes (old hashes) | consolidated step |
|---|---|
| 1a59e61cc82d tooling, f3fcc325e521, 145b058805c1 STATS-1, ad0623610ef6 stock controls | `f2ecf2b51e4f` |
| def2cf34faeb SWMR-1, a633cc3f0e77 STC-1/2 | `4bb688790f44` |
| 667343f20eaf EA-1, 7c35430b549d / dbde56f2e3f7 capture, 58c8ace31c52 PASS-2, d06851ec7a6d EA-2, cbeb570ed61c EA-3, 92c681a2642f EA-4, 7be7dc568f45, 957f8e506697 EA-5, 09b9a74d17b7 EA-6, 67e9532aa926, feb4f802b35b, fe9c96cfd10f EA-9, c402aec8dec3 UAF, bfaa2b859100 EA-7/8 | `9c040aef640a` |
| 12399f969130 LO-1/4, 20c178992bfd LO-2, c8debe12f363 LO-3/5 | `4d631b5e4563` |
| 450fc39a8545 DE-5, 2aa46b60cb86 DE-2, 3b23aa757035 DE-3, 223406c284b6 RT-1, dc7d9e0a8a20 DE-4, 4df6138b85ca PASS-3 | `bca50773de9a` |
| 5eca63015f63 P3, 0d63553b3e43 RT-2, 248f45c260ef evict_watch | `17f24744af64` |
| fafbebedb41e S, 9ef1fce44e2d PASS-1 | `6a2aed2d00e7` |
| 44da093b1b45, babd3b0a01df, d6ea6277e35c tests; every ledger commit | `4ca57ec9c3a4` |
