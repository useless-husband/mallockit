#!/usr/bin/env python3
"""Ablation study: turn one design decision off at a time and measure.

Builds variants of the shared library (compile-time switches from
src/internal.h) and runs a few workloads under each, plus run-time switches
given through environment variables. Results go to
results/ablation/ablation.jsonl and are summarised in docs/report.md.

  python3 tools/ablation.py --ext build/ext
"""
import argparse
import json
import os
import random
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "bench"))
import run as bench  # noqa: E402

VARIANTS = [
    # name, extra -D flags, extra environment, what it removes
    ("baseline", [], {}, "everything on"),
    ("no-local-free", ["-DMK_LOCAL_FREE=0"], {}, "owner frees go straight back to page->free"),
    ("no-keep-last-page", ["-DMK_KEEP_LAST_PAGE=0"], {}, "an empty page is always retired at once"),
    ("no-adopt", ["-DMK_ADOPT=0"], {}, "new threads never adopt the heap of an exited thread"),
    ("no-mmap-hint", ["-DMK_MMAP_HINT=0"], {}, "every new segment is mapped, unmapped, over-mapped and trimmed"),
    ("tiny-cache", ["-DMK_CACHE_MAX=1"], {}, "segment cache holds one segment"),
    ("tsd-pthread", [], {"MALLOCKIT_NO_TSD_DIRECT": "1"}, "heap lookup through pthread_getspecific()"),
    ("purge-now", [], {"MALLOCKIT_PURGE_DELAY": "0"}, "free pages are purged immediately"),
    ("purge-never", [], {"MALLOCKIT_PURGE_DELAY": "-1"}, "free memory is never purged"),
]


def build_variant(outdir, name, flags):
    os.makedirs(outdir, exist_ok=True)
    lib = os.path.join(outdir, f"libmallockit-{name}.{bench.SO}")
    shared = ["-dynamiclib"] if bench.IS_MAC else ["-shared"]
    cmd = [os.environ.get("CC", "cc"), "-std=c11", "-O2", "-fPIC", "-DMK_OVERRIDE=1", "-fno-builtin-malloc",
           "-fno-builtin-calloc", "-fno-builtin-realloc", "-fno-builtin-free"] + flags + shared + \
          [os.path.join(ROOT, "src", "mallockit.c"), "-lpthread", "-o", lib]
    if not bench.IS_MAC:
        cmd[1:1] = ["-D_DEFAULT_SOURCE", "-D_GNU_SOURCE"]
    subprocess.run(cmd, check=True)
    return lib


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ext", default=os.path.join(ROOT, "build", "ext"))
    ap.add_argument("--out", default=os.path.join(ROOT, "results", "ablation"))
    ap.add_argument("--libdir", default=os.path.join(ROOT, "build", "ablation"))
    ap.add_argument("--reps", type=int, default=3)
    args = ap.parse_args()
    libs = {}
    for name, flags, env, _ in VARIANTS:
        libs[name] = build_variant(args.libdir, name, flags) if (flags or name == "baseline") else None
    for name, flags, env, _ in VARIANTS:
        if libs[name] is None:
            libs[name] = libs["baseline"]
    wl = [w for w in bench.workloads(args.ext, [4], quick=True)
          if (w[0], w[1]) in {("glibc-simple", 1), ("espresso", 1), ("malloc-large", 1), ("larson", 4),
                              ("mstress", 4), ("xmalloc-test", 4)}]
    os.makedirs(args.out, exist_ok=True)
    rng = random.Random(7)
    with open(os.path.join(args.out, "ablation.jsonl"), "w") as f:
        f.write(json.dumps({"machine": bench.machine(), "started": time.strftime("%Y-%m-%d %H:%M:%S"),
                            "variants": [{"name": v[0], "flags": v[1], "env": v[2], "removes": v[3]}
                                         for v in VARIANTS]}) + "\n")
        for name, t, argv, metric, parser in wl:
            for rep in range(args.reps):
                order = list(VARIANTS)
                rng.shuffle(order)
                for vname, _, env, _ in order:
                    r = bench.run_measured(argv, libs[vname], extra_env=env, cwd=os.path.dirname(argv[0]))
                    val = (parser(r["out"]) if parser else r["wall"]) if r["ok"] else None
                    rec = {"bench": name, "threads": t, "variant": vname, "rep": rep, "metric": metric,
                           "ok": r["ok"] and val is not None, "value": val, "maxrss": r.get("maxrss"),
                           "footprint": r.get("footprint"),
                           "load1": r.get("load1")}
                    f.write(json.dumps(rec) + "\n")
                    f.flush()
                    print(f"{name:13s} {vname:18s} rep {rep}: {val}  mem {(r.get('footprint') or 0) >> 20} MiB",
                          flush=True)


if __name__ == "__main__":
    main()
