#!/usr/bin/env python3
"""Render docs/results.html from the measurement files in results/.

A static page: inline CSS and SVG, all numbers in the page itself, no
external resources and no scripts (it opens in Safari's Lockdown Mode).
Every chart has a table with the same numbers next to it.

  python3 analysis/page.py --results results --out docs/results.html
"""
import argparse
import collections
import html
import json
import math
import os
import statistics

ALLOCS = ["mallockit", "system", "mimalloc", "jemalloc"]
SERIES = {a: f"s{i}" for i, a in enumerate(ALLOCS)}
LABEL = {"mallockit": "mallockit", "system": "macOS system", "mimalloc": "mimalloc 2.2.4", "jemalloc": "jemalloc 5.3.0"}

CSS = """
:root{--bg:#fcfcfb;--fg:#0b0b0b;--fg2:#52514e;--line:#dcdbd6;--grid:#ecebe7;--ok:#0b7a0b;--bad:#c62f2f;
--s0:#2a78d6;--s1:#eb6834;--s2:#1baf7a;--s3:#eda100}
@media (prefers-color-scheme: dark){:root:not([data-theme="light"]){--bg:#1a1a19;--fg:#fff;--fg2:#c3c2b7;--line:#383835;
--grid:#2a2a28;--ok:#3fbf3f;--bad:#e66767;--s0:#3987e5;--s1:#d95926;--s2:#199e70;--s3:#c98500}}
:root[data-theme="dark"]{--bg:#1a1a19;--fg:#fff;--fg2:#c3c2b7;--line:#383835;--grid:#2a2a28;--ok:#3fbf3f;--bad:#e66767;
--s0:#3987e5;--s1:#d95926;--s2:#199e70;--s3:#c98500}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.5 -apple-system,BlinkMacSystemFont,"Helvetica Neue",Arial,sans-serif}
main{max-width:1000px;padding:24px 16px 64px}
h1{font-size:24px;margin:0 0 4px}h2{font-size:19px;margin:36px 0 8px;border-top:1px solid var(--line);padding-top:20px}
h3{font-size:15px;margin:18px 0 6px}
p,li{max-width:78ch}.muted{color:var(--fg2)}
table{border-collapse:collapse;font-size:13px;margin:8px 0;display:block;overflow-x:auto;max-width:100%}
th,td{text-align:left;padding:4px 12px 4px 0;border-bottom:1px solid var(--line);white-space:nowrap;vertical-align:top}
td.num,th.num{text-align:right;font-variant-numeric:tabular-nums}
.ok{color:var(--ok)}.bad{color:var(--bad);font-weight:600}.best{font-weight:700}
.charts{display:flex;flex-wrap:wrap;gap:8px 24px}
.chart{width:100%;max-width:470px;height:auto;display:block}
.chart .grid{stroke:var(--grid);stroke-width:1}.chart .axis{stroke:var(--line);stroke-width:1}
.chart text{fill:var(--fg2);font-size:11px}.chart .title{fill:var(--fg);font-size:13px;font-weight:600}
.chart .pcore{stroke:var(--fg2);stroke-width:1;stroke-dasharray:3 3}
.chart .ln{fill:none;stroke-width:2}.chart .pt{stroke:var(--bg);stroke-width:2}
.chart .bar{stroke:var(--bg);stroke-width:2}
.s0{stroke:var(--s0);fill:var(--s0)}.s1{stroke:var(--s1);fill:var(--s1)}.s2{stroke:var(--s2);fill:var(--s2)}.s3{stroke:var(--s3);fill:var(--s3)}
.ln.s0,.ln.s1,.ln.s2,.ln.s3{fill:none}
.legend{display:flex;flex-wrap:wrap;gap:4px 16px;font-size:13px;margin:6px 0}
.legend span::before{content:"";display:inline-block;width:10px;height:10px;border-radius:5px;margin-right:6px;background:var(--c)}
code{font-size:13px}
"""


def esc(s):
    return html.escape(str(s))


def load_jsonl(path):
    if not os.path.exists(path):
        return [], []
    heads, rows = [], []
    for line in open(path):
        r = json.loads(line)
        (rows if "bench" in r else heads).append(r)
    return heads, rows


def med(xs):
    return statistics.median(xs) if xs else None


def spread(xs):
    """(min, max) of the repetitions."""
    return (min(xs), max(xs)) if xs else (None, None)


def fmt(v, metric):
    if v is None:
        return "–"
    if metric == "time":
        return f"{v:.3f} s"
    if v >= 1e9:
        return f"{v / 1e9:.2f} G/s"
    return f"{v / 1e6:.1f} M/s"


def mib(b):
    return "–" if b is None else f"{b / 2**20:.0f}"


def legend():
    return '<div class="legend">' + "".join(
        f'<span style="--c:var(--{SERIES[a]})">{esc(LABEL[a])}</span>' for a in ALLOCS) + "</div>"


def line_chart(title, ylabel, xs, series, pcores=4):
    """series: alloc -> list of (x, y, lo, hi)."""
    W, H, L, R, T, B = 470, 260, 56, 86, 26, 34
    ys = [p[3] if p[3] is not None else p[1] for s in series.values() for p in s if p[1] is not None]
    if not ys:
        return ""
    ymax = max(ys) * 1.08
    xmin, xmax = min(xs), max(xs)
    sx = lambda x: L + (x - xmin) / (xmax - xmin or 1) * (W - L - R)  # noqa: E731
    sy = lambda y: T + (1 - y / ymax) * (H - T - B)  # noqa: E731
    out = [f'<svg class="chart" viewBox="0 0 {W} {H}" role="img" aria-label="{esc(title)}">',
           f'<text class="title" x="{L}" y="15">{esc(title)}</text>']
    for k in range(5):
        y = ymax * k / 4
        out.append(f'<line class="grid" x1="{L}" x2="{W - R}" y1="{sy(y):.1f}" y2="{sy(y):.1f}"/>')
        lab = f"{y / 1e6:.0f}M" if ymax > 1e6 else (f"{y:.2f}" if ymax < 10 else f"{y:.0f}")
        out.append(f'<text x="{L - 6}" y="{sy(y) + 4:.1f}" text-anchor="end">{lab}</text>')
    for x in xs:
        out.append(f'<text x="{sx(x):.1f}" y="{H - B + 15}" text-anchor="middle">{x}</text>')
    out.append(f'<text x="{(L + W - R) / 2:.0f}" y="{H - 4}" text-anchor="middle">threads</text>')
    out.append(f'<text x="12" y="{(T + H - B) / 2:.0f}" transform="rotate(-90 12 {(T + H - B) / 2:.0f})" '
               f'text-anchor="middle">{esc(ylabel)}</text>')
    if xmin <= pcores <= xmax:
        out.append(f'<line class="pcore" x1="{sx(pcores):.1f}" x2="{sx(pcores):.1f}" y1="{T}" y2="{H - B}"/>')
        out.append(f'<text x="{sx(pcores) + 4:.1f}" y="{T + 10}">4 P cores</text>')
    labels = []
    for a in ALLOCS:
        pts = [p for p in series.get(a, []) if p[1] is not None]
        if not pts:
            continue
        cls = SERIES[a]
        d = " ".join(f"{'M' if i == 0 else 'L'}{sx(p[0]):.1f},{sy(p[1]):.1f}" for i, p in enumerate(pts))
        out.append(f'<path class="ln {cls}" d="{d}"/>')
        for p in pts:
            tip = f"{LABEL[a]}, {p[0]} threads: median {p[1]:.4g}" + (
                f" (range {p[2]:.4g} – {p[3]:.4g})" if p[2] is not None else "")
            out.append(f'<circle class="pt {cls}" cx="{sx(p[0]):.1f}" cy="{sy(p[1]):.1f}" r="4">'
                       f'<title>{esc(tip)}</title></circle>')
        labels.append([sy(pts[-1][1]), a])
    labels.sort()
    for i in range(1, len(labels)):  # keep end labels apart
        if labels[i][0] - labels[i - 1][0] < 12:
            labels[i][0] = labels[i - 1][0] + 12
    for y, a in labels:
        out.append(f'<text x="{W - R + 6}" y="{y + 4:.1f}">{esc(a)}</text>')
    out.append("</svg>")
    return "".join(out)


def bar_chart(title, groups, values, unit="MiB"):
    """groups: list of names; values: (group, alloc) -> value."""
    W, L, R, T = 470, 110, 50, 26
    bh, gap = 9, 12
    H = T + len(groups) * (len(ALLOCS) * bh + gap) + 20
    vmax = max([v for v in values.values() if v] or [1])
    sx = lambda v: L + v / vmax * (W - L - R)  # noqa: E731
    out = [f'<svg class="chart" viewBox="0 0 {W} {H}" role="img" aria-label="{esc(title)}">',
           f'<text class="title" x="0" y="15">{esc(title)}</text>']
    y = T
    for g in groups:
        out.append(f'<text x="{L - 8}" y="{y + len(ALLOCS) * bh / 2 + 4:.1f}" text-anchor="end">{esc(g)}</text>')
        for a in ALLOCS:
            v = values.get((g, a))
            if v:
                out.append(f'<rect class="bar {SERIES[a]}" x="{L}" y="{y}" width="{max(sx(v) - L, 2):.1f}" '
                           f'height="{bh}" rx="2"><title>{esc(f"{g}, {LABEL[a]}: {v:.0f} {unit}")}</title></rect>')
                out.append(f'<text x="{max(sx(v), L + 2) + 4:.1f}" y="{y + bh - 0.5}" style="font-size:9px">{v:.0f}</text>')
            y += bh
        y += gap
    out.append(f'<line class="axis" x1="{L}" x2="{L}" y1="{T - 4}" y2="{y - gap + 4}"/>')
    out.append(f'<text x="{W - R}" y="{H - 4}" text-anchor="end">{vmax:.0f} {unit}</text>')
    out.append("</svg>")
    return "".join(out)


def summarize(rows, tag=None):
    d = collections.defaultdict(list)
    mem = collections.defaultdict(list)
    for r in rows:
        if not r.get("ok") or (tag and r.get("tag") != tag):
            continue
        k = (r["bench"], r["threads"], r["alloc"])
        d[k].append(r["value"])
        mem[k].append(r.get("footprint") or r.get("maxrss") or 0)
    return d, mem


def section_bench(rows, heads):
    if not rows:
        return "<p>No benchmark results yet (run <code>make bench</code>).</p>"
    metric = {r["bench"]: r["metric"] for r in rows}
    d, mem = summarize([r for r in rows if r.get("tag") != "ecore"])
    out = []
    # ----- single-thread + 4-thread summary table
    out.append("<h2>1. Workloads against other allocators</h2>")
    out.append("<p>Median of the repetitions; ±x% is half the min – max range relative to the median. Time: lower is better; "
               "throughput (/s): higher is better. Memory is the peak physical footprint in MiB "
               "(see method). The best value in each row is bold. "
               "<span class='muted'>The machine was shared with other work during the runs; the load "
               "average is in the raw data.</span></p>")
    cases = sorted({(b, t) for (b, t, a) in d}, key=lambda x: (x[1] != 1, x[0], x[1]))
    cases = [c for c in cases if c[1] in (1, 4) and c[0] != "cache-scratch1"]  # = cache-scratch, 1 thread
    out.append(legend())
    out.append("<table><tr><th>workload</th><th class=num>threads</th>" + "".join(
        f"<th class=num>{esc(a)}</th>" for a in ALLOCS) +
        "<th class=num>peak MiB: " + " / ".join(a[:2] if a != "mallockit" else "mk" for a in ALLOCS) + "</th></tr>")
    for b, t in cases:
        m = metric[b]
        meds = {a: med(d.get((b, t, a), [])) for a in ALLOCS}
        valid = [v for v in meds.values() if v is not None]
        best = (min(valid) if m == "time" else max(valid)) if valid else None
        cells = []
        for a in ALLOCS:
            v = meds[a]
            lo, hi = spread(d.get((b, t, a), []))
            rng = f" <span class=muted>±{(hi - lo) / 2 / v * 100:.0f}%</span>" if v and lo is not None else ""
            cls = "num best" if v is not None and v == best else "num"
            cells.append(f"<td class='{cls}'>{fmt(v, m)}{rng}</td>")
        mems = {a: med(mem.get((b, t, a), [])) for a in ALLOCS}
        mv = [v for v in mems.values() if v]
        mbest = min(mv) if mv else None
        cells.append("<td class=num>" + " / ".join(
            (f"<b>{mib(mems[a])}</b>" if mems[a] and mems[a] == mbest else mib(mems[a])) for a in ALLOCS) + "</td>")
        out.append(f"<tr><td>{esc(b)}</td><td class=num>{t}</td>{''.join(cells)}</tr>")
    out.append("</table>")
    # ----- scaling charts
    scal = sorted({b for (b, t, a) in d if t != 1 or b in ("larson", "mstress", "xmalloc-test")})
    scal = [b for b in scal if len({t for (bb, t, a) in d if bb == b}) > 1]
    if scal:
        out.append("<h2>2. Scaling with threads (4 performance + 6 efficiency cores)</h2>")
        out.append("<p>Median for every thread count (throughput, or run time for the timed workloads; "
                   "in mstress every thread does a fixed amount of work, so its time grows with the "
                   "thread count). The dashed line marks 4 threads: up to there macOS can "
                   "keep every thread on a performance core; beyond it threads also run on the "
                   "efficiency cores, which are slower, so curves flatten or bend there for every "
                   "allocator. Hover a point for the median and range.</p>")
        out.append(legend())
        out.append('<div class="charts">')
        for b in scal:
            m = metric[b]
            xs = sorted({t for (bb, t, a) in d if bb == b})
            series = {}
            for a in ALLOCS:
                pts = []
                for t in xs:
                    v = d.get((b, t, a), [])
                    if not v:
                        continue
                    pts.append((t, med(v), min(v), max(v)))
                series[a] = pts
            ylabel = "seconds (lower is better)" if m == "time" else "operations/s (higher is better)"
            out.append(line_chart(b, ylabel, xs, series))
        out.append("</div>")
        out.append("<details><summary>Table of the scaling numbers</summary><table><tr><th>workload</th>"
                   "<th class=num>threads</th>" + "".join(f"<th class=num>{esc(a)}</th>" for a in ALLOCS) +
                   "".join(f"<th class=num>mem {esc(a)}</th>" for a in ALLOCS) + "</tr>")
        for b in scal:
            for t in sorted({t for (bb, t, a) in d if bb == b}):
                out.append(f"<tr><td>{esc(b)}</td><td class=num>{t}</td>" + "".join(
                    f"<td class=num>{fmt(med(d.get((b, t, a), [])), metric[b])}</td>" for a in ALLOCS) + "".join(
                    f"<td class=num>{mib(med(mem.get((b, t, a), [])))}</td>" for a in ALLOCS) + "</tr>")
        out.append("</table></details>")
    # ----- memory chart
    groups = [b for b, t in cases if t == (1 if b in ("cfrac", "espresso", "glibc-simple", "malloc-large",
                                                      "cache-scratch1") else 4)]
    vals = {}
    for b in groups:
        t = 1 if b in ("cfrac", "espresso", "glibc-simple", "malloc-large", "cache-scratch1") else 4
        for a in ALLOCS:
            v = med(mem.get((b, t, a), []))
            if v:
                vals[(b, a)] = v / 2**20
    big = [g for g in groups if max(vals.get((g, a), 0) for a in ALLOCS) > 20]
    if big:
        out.append("<h3>Peak memory (physical footprint) of the memory-heavy workloads</h3>")
        out.append(bar_chart("peak footprint, MiB (shorter is better)", big, vals))
    # ----- e-cores
    de, _ = summarize(rows, tag="ecore")
    if de:
        out.append("<h2>3. The same single-thread workloads on the efficiency cores</h2>")
        out.append("<p>Run with darwin background priority (<code>PRIO_DARWIN_BG</code>), which keeps a "
                   "process on the efficiency cores <em>and</em> lowers their clock, so this is the worst "
                   "case a background thread sees, not the E cores at full speed. Median of 3.</p>")
        out.append("<table><tr><th>workload</th>" + "".join(
            f"<th class=num>{esc(a)} P (default)</th><th class=num>{esc(a)} E (background)</th>"
            for a in ALLOCS if any(k[2] == a for k in de)) + "</tr>")
        for b in sorted({k[0] for k in de}):
            cells = ""
            for a in ALLOCS:
                if not any(k[2] == a for k in de):
                    continue
                p = med(d.get((b, 1, a), []))
                e = med(de.get((b, 1, a), []))
                ratio = f" <span class=muted>(×{e / p:.1f})</span>" if p and e else ""
                cells += f"<td class=num>{fmt(p, 'time')}</td><td class=num>{fmt(e, 'time')}{ratio}</td>"
            out.append(f"<tr><td>{esc(b)}</td>{cells}</tr>")
        out.append("</table>")
    return "".join(out)


def section_score(path):
    if not os.path.exists(path):
        return ""
    s = json.load(open(path))
    out = ["<h2>4. 6.172-style score: utilisation and throughput on traces</h2>",
           "<p>Seven generated traces (see <code>bench/score.py</code>) replayed by "
           "<code>build/trace_replay</code>. Utilisation U = peak live payload / growth of the peak "
           "physical footprint during the replay (1.0 would be no overhead at all). Throughput T is "
           f"relative to the system allocator, capped at {s['t_cap']:g}. Score = geometric mean over "
           f"traces of U<sup>{s['w']}</sup>·T<sup>{1 - s['w']}</sup>. "
           "The course used its own traces and weights; this is the same idea, not its grader.</p>"]
    out.append("<table><tr><th>allocator</th><th class=num>score</th><th class=num>utilisation (geo-mean)</th>"
               "<th class=num>throughput vs system (geo-mean)</th></tr>")
    best = max(v["score"] for v in s["scores"].values())
    for a in ALLOCS:
        v = s["scores"].get(a)
        if v:
            out.append(f"<tr><td>{esc(a)}</td><td class='num{' best' if v['score'] == best else ''}'>"
                       f"{v['score']:.3f}</td><td class=num>{v['util_geomean']:.3f}</td>"
                       f"<td class=num>{v['t_rel_geomean']:.2f}×</td></tr>")
    out.append("</table>")
    rows = s["rows"]
    traces = []
    for r in rows:
        if r["trace"] not in traces:
            traces.append(r["trace"])
    out.append("<table><tr><th>trace</th><th class=num>ops</th><th class=num>peak payload</th>" + "".join(
        f"<th class=num>U {esc(a)}</th>" for a in ALLOCS) + "".join(
        f"<th class=num>Mops/s {esc(a)}</th>" for a in ALLOCS) + "</tr>")
    for t in traces:
        by = {r["alloc"]: r for r in rows if r["trace"] == t}
        any_r = next(iter(by.values()))
        out.append(f"<tr><td>{esc(t)}</td><td class=num>{any_r['ops']:,}</td>"
                   f"<td class=num>{any_r['peak_payload'] / 2**20:.1f} MiB</td>" + "".join(
            f"<td class=num>{by[a]['util']:.2f}</td>" if a in by else "<td>–</td>" for a in ALLOCS) + "".join(
            f"<td class=num>{by[a]['ops_per_sec'] / 1e6:.1f}</td>" if a in by else "<td>–</td>" for a in ALLOCS)
                   + "</tr>")
    out.append("</table>")
    return "".join(out)


def section_ablation(path):
    heads, rows = load_jsonl(path)
    if not rows:
        return ""
    variants = heads[0]["variants"] if heads else []
    d = collections.defaultdict(list)
    m = collections.defaultdict(list)
    metric = {}
    for r in rows:
        if r["ok"]:
            d[(r["bench"], r["variant"])].append(r["value"])
            m[(r["bench"], r["variant"])].append(r.get("footprint") or r.get("maxrss") or 0)
            metric[r["bench"]] = r["metric"]
    benches = sorted(metric)
    out = ["<h2>5. Ablation: one design decision off at a time</h2>",
           "<p>Each row turns one thing off (compile-time switch or environment variable) and runs the "
           "same workloads, 3 repetitions, interleaved. Cells: change in speed versus the full "
           "allocator (positive = faster), then peak footprint in MiB. Differences of a few percent "
           "are within the noise of this shared machine.</p>",
           "<table><tr><th>variant</th><th>what is removed</th>" + "".join(
               f"<th class=num>{esc(b)}</th>" for b in benches) + "</tr>"]
    for v in variants:
        cells = ""
        for b in benches:
            base = med(d.get((b, "baseline"), []))
            x = med(d.get((b, v["name"]), []))
            mm = med(m.get((b, v["name"]), []))
            if not base or not x:
                cells += "<td>–</td>"
                continue
            speed = (base / x - 1) if metric[b] == "time" else (x / base - 1)
            cls = "bad" if speed < -0.05 else ""
            cells += (f"<td class='num {cls}'>{speed * 100:+.0f}% <span class=muted>{mib(mm)}</span></td>"
                      if v["name"] != "baseline" else f"<td class=num>{fmt(x, metric[b])} "
                                                      f"<span class=muted>{mib(mm)}</span></td>")
        out.append(f"<tr><td>{esc(v['name'])}</td><td class=muted>{esc(v['removes'])}</td>{cells}</tr>")
    out.append("</table>")
    return "".join(out)


def section_realprogs(path):
    if not os.path.exists(path):
        return ""
    s = json.load(open(path))
    runs = s["runs"]
    progs = []
    for r in runs:
        if r["program"] not in progs:
            progs.append(r["program"])
    out = ["<h2>6. Real programs on top of mallockit</h2>",
           "<p>Unmodified programs with the allocator injected. <em>answer</em> is a digest of each "
           "program's output (or the test-suite verdict); it must be the same for every allocator. "
           "<code>mallockit-debug</code> is the guard-mode build: any overrun, double free or write "
           "after free in the program would be reported.</p>",
           "<table><tr><th>program</th><th>allocator</th><th class=num>time (median)</th>"
           "<th class=num>peak MiB</th><th>answer</th><th>same as system</th><th>notes</th></tr>"]
    for p in progs:
        allocs = []
        for r in runs:
            if r["program"] == p and r["alloc"] not in allocs:
                allocs.append(r["alloc"])
        sys_ans = next((r.get("answer") for r in runs if r["program"] == p and r["alloc"] == "system"), None)
        for a in allocs:
            rr = [r for r in runs if r["program"] == p and r["alloc"] == a]
            ok = all(r["ok"] for r in rr)
            ans = rr[0].get("answer")
            same = ans == sys_ans and ans is not None
            note = ""
            if p == "cpython" and rr[0].get("cpython"):
                c = rr[0]["cpython"]
                note = f"{c['ok']} test modules OK" + (f", failed: {' '.join(c['failed'])}" if c["failed"] else "")
            if rr[0].get("guard_report"):
                note += " guard: " + "; ".join(rr[0]["guard_report"])
            if not ok:
                note += " exit status non-zero"
            out.append(f"<tr><td>{esc(p)}</td><td>{esc(a)}</td>"
                       f"<td class=num>{med([r['wall'] for r in rr if r.get('wall')]) or 0:.2f} s</td>"
                       f"<td class=num>{mib(med([r.get('footprint') or 0 for r in rr]))}</td>"
                       f"<td><code>{esc(ans)}</code></td><td class={'ok' if same else 'bad'}>"
                       f"{'yes' if same else 'NO'}</td><td class=muted>{esc(note)}</td></tr>")
    out.append("</table>")
    return "".join(out)


def section_verification(results):
    out = ["<h2>7. Verification</h2>"]
    vpath = os.path.join(results, "verification.json")
    if os.path.exists(vpath):
        v = json.load(open(vpath))
        out.append("<table><tr><th>check</th><th>command</th><th>result</th></tr>")
        for c in v["checks"]:
            out.append(f"<tr><td>{esc(c['name'])}</td><td><code>{esc(c['command'])}</code></td>"
                       f"<td class={'ok' if c['passed'] else 'bad'}>{esc(c['result'])}</td></tr>")
        out.append("</table>")
    mpath = os.path.join(results, "mutants", "mutants.json")
    if os.path.exists(mpath):
        ms = json.load(open(mpath))
        killed = sum(m["status"] == "killed" for m in ms)
        out.append(f"<h3>Mutation checks: {killed} of {len(ms)} planted bugs caught</h3>")
        out.append("<table><tr><th>mutant</th><th>simulated bug</th><th>caught by</th><th>status</th></tr>")
        for m in ms:
            by = ", ".join(m.get("failed_tests", [])[:3]) + (" (TSan data race)" if m.get("tsan_race") else "")
            out.append(f"<tr><td>{esc(m['name'])}</td><td>{esc(m.get('simulates', ''))}</td>"
                       f"<td class=muted>{esc(by)}</td><td class={'ok' if m['status'] == 'killed' else 'bad'}>"
                       f"{esc(m['status'])}</td></tr>")
        out.append("</table>")
    return "".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", default="results")
    ap.add_argument("--out", default="docs/results.html")
    args = ap.parse_args()
    heads, rows = load_jsonl(os.path.join(args.results, "full", "bench.jsonl"))
    mach = heads[0]["machine"] if heads else {}
    started = heads[0].get("started", "") if heads else ""
    body = [f"<h1>mallockit: measured results</h1>",
            f"<p class=muted>{esc(mach.get('cpu', ''))}, {esc(mach.get('pcores', '?'))} performance + "
            f"{esc(mach.get('ecores', '?'))} efficiency cores, {int(mach.get('mem', 0) or 0) >> 30} GB, "
            f"{esc(mach.get('pagesize', ''))}-byte pages, {esc(mach.get('platform', ''))}. Measured "
            f"{esc(started)}. Allocators are injected with <code>DYLD_INSERT_LIBRARIES</code> into the "
            "same unmodified binaries; mimalloc and jemalloc were built from source at pinned versions "
            "(<code>bench/fetch.sh</code>). Raw data: <code>results/</code>; method: "
            "<code>docs/report.md</code>.</p>",
            "<p><strong>Method.</strong> Workloads from mimalloc-bench (larson, mstress, xmalloc-test, "
            "cache-scratch, glibc-simple/thread, malloc-large, cfrac, espresso), each repetition runs every "
            "allocator once in a shuffled order. Memory is the peak <em>physical footprint</em> "
            "(<code>proc_pid_rusage</code>), because on macOS the resident set still counts pages an "
            "allocator has handed back with <code>MADV_FREE_REUSABLE</code>. jemalloc on macOS runs as a "
            "malloc zone (its default there), so every call pays libmalloc's zone dispatch; mallockit and "
            "mimalloc interpose <code>malloc</code> directly. The machine was shared with other jobs.</p>"]
    body.append(section_bench(rows, heads))
    body.append(section_score(os.path.join(args.results, "score", "score.json")))
    body.append(section_ablation(os.path.join(args.results, "ablation", "ablation.jsonl")))
    body.append(section_realprogs(os.path.join(args.results, "realprogs", "realprogs.json")))
    body.append(section_verification(args.results))
    page = ("<!doctype html><html lang=en><head><meta charset=utf-8>"
            "<meta name=viewport content='width=device-width,initial-scale=1'>"
            f"<title>mallockit results</title><style>{CSS}</style></head><body><main>"
            + "".join(body) + "</main></body></html>\n")
    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w") as fh:
        fh.write(page)
    print("wrote", args.out, f"({len(page) // 1024} KiB)")


if __name__ == "__main__":
    main()
