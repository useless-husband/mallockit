#!/usr/bin/env python3
"""Print the Markdown tables used in README.md and docs/report.md from the
raw results, so the numbers in the documents can be regenerated.

  python3 analysis/tables.py [--results results]
"""
import argparse
import collections
import json
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import page  # noqa: E402

ALLOCS = page.ALLOCS


def fmt(v, metric):
    if v is None:
        return "–"
    if metric == "time":
        return f"{v:.2f} s"
    return f"{v / 1e6:.0f} M/s"


def bench_tables(results):
    heads, rows = page.load_jsonl(os.path.join(results, "full", "bench.jsonl"))
    if not rows:
        return
    metric = {r["bench"]: r["metric"] for r in rows}
    d, mem = page.summarize([r for r in rows if r.get("tag") != "ecore"])
    print("### Workloads (median; memory = peak footprint MiB)\n")
    print("| workload | threads | " + " | ".join(ALLOCS) + " | memory " + " / ".join(ALLOCS) + " |")
    print("|---|---:|" + "---:|" * len(ALLOCS) + "---:|")
    cases = sorted({(b, t) for (b, t, a) in d})
    for b, t in cases:
        if t not in (1, 4, 10):
            continue
        meds = {a: page.med(d.get((b, t, a), [])) for a in ALLOCS}
        vals = [v for v in meds.values() if v is not None]
        best = (min(vals) if metric[b] == "time" else max(vals)) if vals else None
        cells = [("**" + fmt(meds[a], metric[b]) + "**") if meds[a] == best else fmt(meds[a], metric[b])
                 for a in ALLOCS]
        mems = " / ".join(page.mib(page.med(mem.get((b, t, a), []))) for a in ALLOCS)
        print(f"| {b} | {t} | " + " | ".join(cells) + f" | {mems} |")
    print()
    # relative to system, geometric mean over the cases shown above
    import math
    print("### mallockit relative to each allocator (geometric mean of speed ratios over all cases)\n")
    for other in ALLOCS[1:]:
        ratios = []
        for b, t in cases:
            x, y = page.med(d.get((b, t, "mallockit"), [])), page.med(d.get((b, t, other), []))
            if x and y:
                ratios.append(y / x if metric[b] == "time" else x / y)
        if ratios:
            g = math.exp(sum(math.log(r) for r in ratios) / len(ratios))
            wins = sum(r > 1.0 for r in ratios)
            print(f"- vs {other}: {g:.2f}× (faster in {wins} of {len(ratios)} cases)")
    print()


def ecore_table(results):
    heads, rows = page.load_jsonl(os.path.join(results, "full", "bench.jsonl"))
    d, _ = page.summarize([r for r in rows if r.get("tag") != "ecore"])
    de, _ = page.summarize(rows, tag="ecore")
    if not de:
        return
    print("### P cores vs efficiency cores (background priority), median seconds\n")
    allocs = [a for a in ALLOCS if any(k[2] == a for k in de)]
    print("| workload | " + " | ".join(f"{a} P | {a} E" for a in allocs) + " |")
    print("|---|" + "---:|---:|" * len(allocs))
    for b in sorted({k[0] for k in de}):
        cells = []
        for a in allocs:
            p, e = page.med(d.get((b, 1, a), [])), page.med(de.get((b, 1, a), []))
            cells.append(f"{p:.2f} | {e:.2f} (×{e / p:.1f})" if p and e else "– | –")
        print(f"| {b} | " + " | ".join(cells) + " |")
    print()


def score_table(results):
    p = os.path.join(results, "score", "score.json")
    if not os.path.exists(p):
        return
    s = json.load(open(p))
    print("### 6.172-style score\n")
    print("| allocator | score | utilisation (geo-mean) | throughput vs system |")
    print("|---|---:|---:|---:|")
    for a in ALLOCS:
        v = s["scores"].get(a)
        if v:
            print(f"| {a} | {v['score']:.3f} | {v['util_geomean']:.3f} | {v['t_rel_geomean']:.2f}× |")
    print()
    print("| trace | " + " | ".join(f"U {a}" for a in ALLOCS) + " | " + " | ".join(f"Mops/s {a}" for a in ALLOCS) + " |")
    print("|---|" + "---:|" * (2 * len(ALLOCS)))
    traces = []
    for r in s["rows"]:
        if r["trace"] not in traces:
            traces.append(r["trace"])
    for t in traces:
        by = {r["alloc"]: r for r in s["rows"] if r["trace"] == t}
        print(f"| {t} | " + " | ".join(f"{by[a]['util']:.2f}" if a in by else "–" for a in ALLOCS) + " | " +
              " | ".join(f"{by[a]['ops_per_sec'] / 1e6:.0f}" if a in by else "–" for a in ALLOCS) + " |")
    print()


def ablation_table(results):
    heads, rows = page.load_jsonl(os.path.join(results, "ablation", "ablation.jsonl"))
    if not rows:
        return
    d = collections.defaultdict(list)
    m = collections.defaultdict(list)
    metric = {}
    for r in rows:
        if r["ok"]:
            d[(r["bench"], r["variant"])].append(r["value"])
            m[(r["bench"], r["variant"])].append(r.get("footprint") or 0)
            metric[r["bench"]] = r["metric"]
    benches = sorted(metric)
    print("### Ablation (speed change vs baseline; peak footprint MiB)\n")
    print("| variant | " + " | ".join(benches) + " |")
    print("|---|" + "---:|" * len(benches))
    for v in heads[0]["variants"]:
        cells = []
        for b in benches:
            base, x = page.med(d.get((b, "baseline"), [])), page.med(d.get((b, v["name"]), []))
            mm = page.mib(page.med(m.get((b, v["name"]), [])))
            if not base or not x:
                cells.append("–")
            elif v["name"] == "baseline":
                cells.append(f"{fmt(x, metric[b])}, {mm}")
            else:
                sp = (base / x - 1) if metric[b] == "time" else (x / base - 1)
                cells.append(f"{sp * 100:+.0f}%, {mm}")
        print(f"| {v['name']} | " + " | ".join(cells) + " |")
    print()


def realprogs_table(results):
    p = os.path.join(results, "realprogs", "realprogs.json")
    if not os.path.exists(p):
        return
    runs = json.load(open(p))["runs"]
    print("### Real programs (median seconds, peak footprint MiB, same answer as system)\n")
    progs = []
    for r in runs:
        if r["program"] not in progs:
            progs.append(r["program"])
    allocs = ["system", "mallockit", "mallockit-debug", "mimalloc", "jemalloc"]
    print("| program | " + " | ".join(allocs) + " |")
    print("|---|" + "---|" * len(allocs))
    for pr in progs:
        sys_ans = next((r.get("answer") for r in runs if r["program"] == pr and r["alloc"] == "system"), None)
        cells = []
        for a in allocs:
            rr = [r for r in runs if r["program"] == pr and r["alloc"] == a]
            if not rr:
                cells.append("–")
                continue
            t = statistics.median([r["wall"] for r in rr if r.get("wall")])
            mm = page.mib(statistics.median([r.get("footprint") or 0 for r in rr]))
            same = "same" if rr[0].get("answer") == sys_ans and sys_ans is not None else "DIFFERENT"
            extra = ""
            if pr == "cpython" and rr[0].get("cpython"):
                c = rr[0]["cpython"]
                extra = f", {c['ok']} OK" + (f", failed {len(c['failed'])}" if c["failed"] else "")
            cells.append(f"{t:.1f} s, {mm} MiB, {same}{extra}")
        print(f"| {pr} | " + " | ".join(cells) + " |")
    print()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", default="results")
    a = ap.parse_args()
    bench_tables(a.results)
    ecore_table(a.results)
    score_table(a.results)
    ablation_table(a.results)
    realprogs_table(a.results)


if __name__ == "__main__":
    main()
