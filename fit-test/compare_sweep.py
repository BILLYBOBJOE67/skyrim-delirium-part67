"""Pairs two sweep CSVs (same trials, two characters) and prints one table per group: A = reference, B = candidate.

usage: python compare_sweep.py <a.csv> <b.csv>
"""
import csv
import sys
from collections import defaultdict
from statistics import mean


def load(p):
    with open(p, newline="") as f:
        return {(r["family"], r["trial"]): r for r in csv.DictReader(f)}


a, b = load(sys.argv[1]), load(sys.argv[2])
keys = sorted(set(a) & set(b))


def num(r, k):
    v = r[k]
    return float(v) if v != "" else None


def avg(rows, k):
    v = [num(r, k) for r in rows if num(r, k) is not None]
    return mean(v) if v else float("nan")


def group(name):
    g = defaultdict(list)
    for k in keys:
        ra, rb = a[k], b[k]
        fam = ra["family"]
        g[(fam, "all")].append((ra, rb))
        if fam in ("push", "shot"):
            m = float(ra["mag"])
            g[(fam, f"{int(m // 60) * 60:>3}-{int(m // 60) * 60 + 59} Ns")].append((ra, rb))
            g[(fam, f"part {ra['part']}")].append((ra, rb))
        if fam != "push":
            g[(fam, ra["stack"])].append((ra, rb))
    return g


print(f"{len(keys)} paired trials;  A {sys.argv[1]}  vs  B {sys.argv[2]}")
print(f"{'group':34} {'n':>4} | {'fall% A/B':>9} {'agree':>5} | {'act A/B':>9} {'late A/B':>9} | {'headmin A/B':>11} | {'moved A/B':>9} | {'settle A/B':>10}")
for (fam, sub), rows in sorted(group(None).items(), key=lambda x: (["push", "shot", "thrown", "lying"].index(x[0][0]), x[0][1] != "all", x[0][1])):
    ra = [r[0] for r in rows]
    rb = [r[1] for r in rows]
    fa, fb = mean(int(r["fell"]) for r in ra) * 100, mean(int(r["fell"]) for r in rb) * 100
    agree = mean(int(x["fell"] == y["fell"]) for x, y in rows) * 100
    fall = f"{fa:3.0f}/{fb:<3.0f}  {agree:4.0f}%" if fam in ("push", "shot") else f"{'':9} {'':5}"
    print(f"{fam + ' ' + sub:34} {len(rows):>4} | {fall} | {avg(ra, 'act'):4.0f}/{avg(rb, 'act'):<4.0f} {avg(ra, 'act_late'):4.0f}/{avg(rb, 'act_late'):<4.0f} | "
          f"{avg(ra, 'head_min'):5.2f}/{avg(rb, 'head_min'):<5.2f} | {avg(ra, 'moved'):4.2f}/{avg(rb, 'moved'):<4.2f} | {avg(ra, 'settle'):4.2f}/{avg(rb, 'settle'):<4.2f}")
bad = [(k, x["finite"], y["finite"]) for k, (x, y) in ((k, (a[k], b[k])) for k in keys) if x["finite"] != "1" or y["finite"] != "1"]
ke = lambda d: sorted(float(d[k]["ke_max"]) for k in keys)
ka, kb = ke(a), ke(b)
print(f"non-finite runs: A {sum(1 for _, x, _ in bad if x != '1')}, B {sum(1 for _, _, y in bad if y != '1')};  "
      f"max KE p50 {ka[len(ka) // 2]:.0f}/{kb[len(kb) // 2]:.0f}  p99 {ka[len(ka) * 99 // 100]:.0f}/{kb[len(kb) * 99 // 100]:.0f}  max {ka[-1]:.0f}/{kb[-1]:.0f} J")
