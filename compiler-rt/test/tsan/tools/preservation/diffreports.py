#!/usr/bin/env python3
"""diffreports.py <reports_root> <stock_cfg> <cfg>...
Per configuration, buckets every test against stock by TSan report keys:
  L1 identical: same set of (kind, both access sites function@file:line, location)
  L2 identical: same set by function only (a frame's line moved) -- "relocated"
  other: anything else, split into lost (stock has a key the config lacks) and
         new (config has a key stock lacks)
Keys come from tsan-exp's tools/preservation/tsan_reports.py so the suite and
application tables read the same. Keys per test are the union over all runs and
RUN lines. Tests are split by whether stock reported anything at all."""
import sys, os, glob, collections
sys.path.insert(0, os.environ.get("TSAN_REPORTS_PY", os.path.dirname(os.path.abspath(__file__))))
import tsan_reports as tr

root, stock = sys.argv[1], sys.argv[2]
cfgs = sys.argv[3:]

def keys_for(cfg):
    out = {}
    d = os.path.join(root, cfg)
    for f in glob.glob(os.path.join(d, "*.cmds")):
        t = os.path.basename(f)[:-5]
        l1, l2, kinds = set(), set(), set()
        for o in glob.glob(os.path.join(d, t + ".k*.out")):
            reps, _ = tr.parse_text(open(o, errors="replace").read(), o)
            for r in reps:
                l1.add(r.key_l1()); l2.add(r.key_l2()); kinds.add(r.kind)
        out[t] = (l1, l2, kinds)
    return out

S = keys_for(stock)
print(f"stock: {len(S)} tests replayed; {sum(1 for v in S.values() if v[0])} reported something, "
      f"{sum(1 for v in S.values() if not v[0])} reported nothing")
for cfg in cfgs:
    C = keys_for(cfg)
    b = collections.Counter(); names = collections.defaultdict(list)
    for t, (sl1, sl2, sk) in sorted(S.items()):
        if t not in C: b["missing"] += 1; names["missing"].append(t); continue
        cl1, cl2, ck = C[t]
        racy = "racy" if sl1 else "noreport"
        if sl1 == cl1: b[(racy, "L1")] += 1
        elif sl2 == cl2: b[(racy, "L2")] += 1; names[(racy, "L2")].append(t)
        else:
            lost, new = sl2 - cl2, cl2 - sl2
            tag = "lost" if lost and not new else ("new" if new and not lost else "lost+new")
            b[(racy, "other:" + tag)] += 1; names[(racy, "other:" + tag)].append(t)
    r = lambda k: b.get(("racy", k), 0); n = lambda k: b.get(("noreport", k), 0)
    oth_r = sum(v for k, v in b.items() if k[0] == "racy" and k[1].startswith("other"))
    oth_n = sum(v for k, v in b.items() if k[0] == "noreport" and k[1].startswith("other"))
    print(f"\n== {cfg} ==  racy tests: L1={r('L1')} L2={r('L2')} other={oth_r} | "
          f"no-report tests: L1={n('L1')} L2={n('L2')} other(new report)={oth_n}")
    for k in sorted(names):
        if k == "missing" or k[1] == "L2" or k[1].startswith("other"):
            print(f"   {k}: {', '.join(names[k])}")
