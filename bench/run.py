#!/usr/bin/env python3
"""Run the allocator benchmark suite.

Every workload is an unmodified program that calls malloc/free; the
allocator under test is injected with DYLD_INSERT_LIBRARIES (macOS) or
LD_PRELOAD (Linux), so every allocator runs the very same binary. The
system allocator is the run without injection.

For each run we record wall time (or the program's own throughput figure),
the peak resident set size from wait4(), and the 1-minute load average,
because the machine may be shared. Allocators are run round-robin inside
each repetition, in a shuffled order, so drifting background load does
not systematically favour one of them.

  python3 bench/run.py --ext build/ext --out results/full
"""
import argparse
import json
import os
import platform
import random
import re
import shlex
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
IS_MAC = platform.system() == "Darwin"
SO = "dylib" if IS_MAC else "so"
PRELOAD = "DYLD_INSERT_LIBRARIES" if IS_MAC else "LD_PRELOAD"


def allocators(ext):
    return {
        "system": None,
        "mallockit": os.path.join(ROOT, "build", "libmallockit." + SO),
        "mimalloc": os.path.join(ext, "mimalloc", "out", "libmimalloc." + SO),
        "jemalloc": os.path.join(ext, "jemalloc-5.3.0", "lib", "libjemalloc." + SO),
    }


def parse_larson(out):
    m = re.search(r"Throughput\s*=\s*([0-9.]+)", out)
    return float(m.group(1)) if m else None


def parse_xmalloc(out):
    m = re.search(r"free/sec:\s*([0-9.]+)\s*M", out)
    return float(m.group(1)) * 1e6 if m else None


def parse_glibc_thread(out):
    m = re.search(r"([0-9.]+)\s+iterations", out)
    return float(m.group(1)) if m else None


def workloads(ext, threads_list, quick):
    """(name, threads, argv, metric, parser). metric 'time' = seconds
    (lower is better); 'ops' = operations per second (higher is better)."""
    mb = os.path.join(ext, "mimalloc-bench", "out")
    src = os.path.join(ext, "mimalloc-bench", "bench")
    secs = "1" if quick else "2"
    w = [
        ("cfrac", 1, [f"{mb}/cfrac", "17545186520507317056371138836327483792789528"], "time", None),
        ("espresso", 1, [f"{mb}/espresso", f"{src}/espresso/largest.espresso"], "time", None),
        ("glibc-simple", 1, [f"{mb}/glibc-simple"], "time", None),
        ("malloc-large", 1, [f"{mb}/malloc-large"], "time", None),
        ("cache-scratch1", 1, [f"{mb}/cache-scratch", "1", "1000", "1", "2000000", "4"], "time", None),
    ]
    for t in threads_list:
        w += [
            ("larson", t, [f"{mb}/larson", secs, "8", "1000", "5000", "100", "4141", str(t)], "ops", parse_larson),
            ("mstress", t, [f"{mb}/mstress", str(t), "50", "25"], "time", None),
            ("xmalloc-test", t, [f"{mb}/xmalloc-test", "-w", str(t), "-t", secs, "-s", "64"], "ops", parse_xmalloc),
            ("cache-scratch", t, [f"{mb}/cache-scratch", str(t), "1000", "1", "2000000", str(t)], "time", None),
            ("glibc-thread", t, [f"{mb}/glibc-thread", str(t)], "ops", parse_glibc_thread),
        ]
    return w


def run_measured(argv, lib, extra_env=None, cwd=None, timeout=600):
    """Run once; reap the child with os.wait4 to get its own peak RSS."""
    env = dict(os.environ)
    env.pop(PRELOAD, None)
    if lib:
        env[PRELOAD] = lib
    if extra_env:
        env.update(extra_env)
    load = os.getloadavg()[0]
    with tempfile.TemporaryFile() as tf:
        t0 = time.perf_counter()
        p = subprocess.Popen(argv, env=env, cwd=cwd, stdout=tf, stderr=subprocess.STDOUT)
        deadline = t0 + timeout
        while True:
            pid, status, ru = os.wait4(p.pid, os.WNOHANG)
            if pid != 0:
                break
            if time.perf_counter() > deadline:
                p.kill()
                os.wait4(p.pid, 0)
                return {"ok": False, "error": "timeout", "load1": load}
            time.sleep(0.002)
        wall = time.perf_counter() - t0
        p.returncode = os.waitstatus_to_exitcode(status)
        tf.seek(0)
        out = tf.read().decode("utf-8", "replace")
    maxrss = ru.ru_maxrss if IS_MAC else ru.ru_maxrss * 1024
    return {"ok": p.returncode == 0, "wall": wall, "maxrss": maxrss, "user": ru.ru_utime, "sys": ru.ru_stime,
            "out": out, "load1": load}


def machine():
    info = {"platform": platform.platform(), "python": platform.python_version()}
    if IS_MAC:
        def sysctl(k):
            try:
                return subprocess.run(["sysctl", "-n", k], capture_output=True, text=True).stdout.strip()
            except OSError:
                return ""
        info.update(cpu=sysctl("machdep.cpu.brand_string"), ncpu=sysctl("hw.ncpu"),
                    pcores=sysctl("hw.perflevel0.logicalcpu"), ecores=sysctl("hw.perflevel1.logicalcpu"),
                    mem=sysctl("hw.memsize"), pagesize=sysctl("hw.pagesize"))
    else:
        info.update(ncpu=os.cpu_count(), pagesize=os.sysconf("SC_PAGESIZE"))
    return info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ext", default=os.path.join(ROOT, "build", "ext"))
    ap.add_argument("--out", default=os.path.join(ROOT, "results", "full"))
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--threads", default="1,2,4,6,8,10")
    ap.add_argument("--only", default="", help="comma-separated workload names")
    ap.add_argument("--allocs", default="system,mallockit,mimalloc,jemalloc")
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--append", action="store_true")
    ap.add_argument("--taskpolicy", default="", help="macOS: e.g. '-b' to run on efficiency cores only")
    ap.add_argument("--tag", default="")
    args = ap.parse_args()

    allocs = {k: v for k, v in allocators(args.ext).items() if k in args.allocs.split(",")}
    for k, v in allocs.items():
        if v and not os.path.exists(v):
            sys.exit(f"missing {k}: {v} (run bench/fetch.sh)")
    threads = [int(x) for x in args.threads.split(",")]
    wl = workloads(args.ext, threads, args.quick)
    if args.only:
        names = set(args.only.split(","))
        wl = [w for w in wl if w[0] in names]
    os.makedirs(args.out, exist_ok=True)
    path = os.path.join(args.out, "bench.jsonl")
    mode = "a" if args.append else "w"
    rng = random.Random(42)
    with open(path, mode) as f:
        f.write(json.dumps({"machine": machine(), "started": time.strftime("%Y-%m-%d %H:%M:%S"),
                            "reps": args.reps, "tag": args.tag}) + "\n")
        for name, t, argv, metric, parser in wl:
            for rep in range(args.reps):
                order = list(allocs.items())
                rng.shuffle(order)
                for alloc, lib in order:
                    cmd = argv
                    if args.taskpolicy and IS_MAC:
                        cmd = ["taskpolicy"] + shlex.split(args.taskpolicy) + argv
                    r = run_measured(cmd, lib, cwd=os.path.dirname(argv[0]))
                    rec = {"bench": name, "threads": t, "alloc": alloc, "rep": rep, "metric": metric,
                           "ok": r["ok"], "wall": r.get("wall"), "maxrss": r.get("maxrss"),
                           "load1": r.get("load1"), "tag": args.tag}
                    if r["ok"]:
                        rec["value"] = parser(r["out"]) if parser else r["wall"]
                        if rec["value"] is None:
                            rec["ok"] = False
                            rec["error"] = "unparsed output: " + r["out"][-200:]
                    else:
                        rec["error"] = r.get("error") or r.get("out", "")[-300:]
                    f.write(json.dumps(rec) + "\n")
                    f.flush()
                    v = rec.get("value")
                    print(f"{name:15s} t={t:<2d} {alloc:9s} rep {rep}: "
                          f"{'FAIL ' + rec.get('error', '')[:60] if not rec['ok'] else ('%.3f s' % v if metric == 'time' else '%.3g ops/s' % v)}"
                          f"  rss {((rec.get('maxrss') or 0) / 2**20):.0f} MiB  load {rec['load1']:.1f}",
                          flush=True)
    print("wrote", path)


if __name__ == "__main__":
    main()
