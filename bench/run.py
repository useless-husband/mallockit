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
    # the program runs for a fixed 2 s (BENCHMARK_DURATION) and prints the
    # total number of malloc+free iterations of all threads
    m = re.search(r"([0-9.]+)\s+iterations", out)
    return float(m.group(1)) / 2.0 if m else None


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
            ("mstress", t, [f"{mb}/mstress", str(t), "50", "200"], "time", None),
            ("xmalloc-test", t, [f"{mb}/xmalloc-test", "-w", str(t), "-t", secs, "-s", "64"], "ops", parse_xmalloc),
            ("cache-scratch", t, [f"{mb}/cache-scratch", str(t), "1000", "1", "2000000", str(t)], "time", None),
            ("glibc-thread", t, [f"{mb}/glibc-thread", str(t)], "ops", parse_glibc_thread),
        ]
    return w


_libc = None


def peak_footprint(pid):
    """macOS: lifetime peak of the physical footprint of an exited, not yet
    reaped child (what Activity Monitor calls memory). On macOS the
    resident set size also counts pages an allocator gave back with
    MADV_FREE_REUSABLE until the kernel takes them, so the footprint is the
    fairer measure of memory use. Returns None elsewhere."""
    global _libc
    if not IS_MAC:
        return None
    import ctypes
    if _libc is None:
        _libc = ctypes.CDLL("/usr/lib/libSystem.dylib")
    buf = (ctypes.c_uint64 * 64)()
    if _libc.proc_pid_rusage(pid, 4, ctypes.byref(buf)) != 0:  # RUSAGE_INFO_V4
        return None
    return int(buf[2 + 28])  # after the 16-byte uuid: ri_lifetime_max_phys_footprint


def _background():
    # macOS: darwin background priority keeps the process on the efficiency
    # cores. Set in the child before exec, so DYLD_* survive (going through
    # /usr/sbin/taskpolicy would strip them: it is SIP-protected).
    os.setpriority(os.PRIO_DARWIN_PROCESS, 0, os.PRIO_DARWIN_BG)


def run_measured(argv, lib, extra_env=None, cwd=None, timeout=600, ecores=False):
    """Run once. Returns wall time, output, peak RSS (wait4) and, on macOS,
    the peak physical footprint (read while the child is a zombie)."""
    env = dict(os.environ)
    env.pop(PRELOAD, None)
    if lib:
        env[PRELOAD] = lib
    if extra_env:
        env.update(extra_env)
    load = os.getloadavg()[0]
    with tempfile.TemporaryFile() as tf:
        t0 = time.perf_counter()
        p = subprocess.Popen(argv, env=env, cwd=cwd, stdout=tf, stderr=subprocess.STDOUT,
                             preexec_fn=_background if (ecores and IS_MAC) else None)
        deadline = t0 + timeout
        while True:
            info = os.waitid(os.P_PID, p.pid, os.WEXITED | os.WNOHANG | os.WNOWAIT)
            if info is not None and info.si_pid == p.pid:
                break
            if time.perf_counter() > deadline:
                p.kill()
                os.wait4(p.pid, 0)
                return {"ok": False, "error": "timeout", "load1": load}
            time.sleep(0.001)
        wall = time.perf_counter() - t0
        foot = peak_footprint(p.pid)
        _, status, ru = os.wait4(p.pid, 0)
        p.returncode = os.waitstatus_to_exitcode(status)
        tf.seek(0)
        out = tf.read().decode("utf-8", "replace")
    maxrss = ru.ru_maxrss if IS_MAC else ru.ru_maxrss * 1024
    return {"ok": p.returncode == 0, "wall": wall, "maxrss": maxrss, "footprint": foot if foot else maxrss,
            "user": ru.ru_utime, "sys": ru.ru_stime, "out": out, "load1": load}


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
    ap.add_argument("--ecores", action="store_true", help="macOS: background priority = efficiency cores only")
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
                    r = run_measured(argv, lib, cwd=os.path.dirname(argv[0]), ecores=args.ecores)
                    rec = {"bench": name, "threads": t, "alloc": alloc, "rep": rep, "metric": metric,
                           "ok": r["ok"], "wall": r.get("wall"), "maxrss": r.get("maxrss"),
                           "footprint": r.get("footprint"),
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
                          f"  mem {((rec.get('footprint') or 0) / 2**20):.0f} MiB  load {rec['load1']:.1f}",
                          flush=True)
    print("wrote", path)


if __name__ == "__main__":
    main()
