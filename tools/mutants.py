#!/usr/bin/env python3
"""Mutation checks: plant one deliberate one-line bug at a time and make
sure the test suite notices (the mutant is "killed").

Each mutant copies src/, include/ and tests/ into a scratch directory,
replaces exactly one line, builds the unit (or guard) tests from the copy
and runs the tests that should catch it. A mutant that survives means the
tests have a blind spot. Results: results/mutants/mutants.json.

  python3 tools/mutants.py
"""
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
IS_MAC = platform.system() == "Darwin"

# name, file, original line (exact, stripped), replacement, build, test filter, what it simulates
MUTANTS = [
    ("sizeclass-rounds-down", "src/internal.h",
     "return 9u + (b - 7u) * 4u + (unsigned)((s >> (b - 2u)) & 3u);",
     "return 8u + (b - 7u) * 4u + (unsigned)((s >> (b - 2u)) & 3u);",
     "unit", "", "size > 128 B gets the bin one step too small"),
    ("remote-free-not-atomic", "src/alloc.c",
     "} while (!atomic_compare_exchange_weak_explicit(&page->xthread_free, &tf, nt, memory_order_release, memory_order_relaxed));",
     "} while (0); *(uintptr_t *)&page->xthread_free = nt;",
     "unit", "stress", "remote free pushes with a plain load/store instead of a CAS"),
    ("remote-free-not-atomic/tsan", "src/alloc.c",
     "} while (!atomic_compare_exchange_weak_explicit(&page->xthread_free, &tf, nt, memory_order_release, memory_order_relaxed));",
     "} while (0); *(uintptr_t *)&page->xthread_free = nt;",
     "tsan", "stress_producer", "same bug, under ThreadSanitizer"),
    ("page-bitmap-off-by-one", "src/segment.c",
     "seg->free_mask &= ~bit;",
     "seg->free_mask &= ~(bit << 1);",
     "unit", "", "marks the neighbouring page as taken, so one page is handed out twice"),
    ("segment-map-off-by-one", "src/segment.c",
     "uintptr_t chunk = (uintptr_t)seg >> MK_SEGMENT_SHIFT;",
     "uintptr_t chunk = ((uintptr_t)seg >> MK_SEGMENT_SHIFT) + 1;",
     "unit", "", "records segments one 4 MiB slot too high in the ownership bitmap"),
    ("aligned-size-not-rounded", "src/alloc.c",
     "size_t sz = mk_align_up(size == 0 ? 1 : size, align);",
     "size_t sz = size == 0 ? 1 : size;",
     "unit", "", "aligned allocation forgets to round the size up to the alignment"),
    ("collect-forgets-used", "src/page.c",
     "page->used -= count;",
     "(void)count;",
     "unit", "", "collecting remote frees does not update the in-use count"),
    ("delayed-free-tag-ignored", "src/alloc.c",
     "if ((tf & MK_TAG_MASK) == MK_DELAYED_USE) {",
     "if (0) {",
     "unit", "", "remote frees into a full page never notify the owner heap"),
    ("realloc-copies-half", "src/alloc.c",
     "memcpy(q, p, usable < size ? usable : size);",
     "memcpy(q, p, (usable < size ? usable : size) / 2);",
     "unit", "", "realloc copies only half of the old contents"),
    ("thread-exit-keeps-owner", "src/heap.c",
     "mk_heap_set_owner(h, 0);",
     "(void)0;",
     "unit", "heap_", "an exited thread's segments keep its thread id"),
    ("guard-canary-unchecked", "src/debug.c",
     "if (c[i] != MK_CANARY) {",
     "if (0) {",
     "guard", "", "guard mode stops checking canaries"),
    ("guard-double-free-unchecked", "src/debug.c",
     "if ((old & bit) == 0) {",
     "if (0) {",
     "guard", "", "guard mode stops checking the allocation bitmap"),
]


def sh(cmd, cwd, timeout=900):
    t0 = time.time()
    p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    return p.returncode, p.stdout.decode("utf-8", "replace"), time.time() - t0


def build_and_test(work, kind, filt):
    cc = os.environ.get("CC", "cc")
    flags = ["-std=c11", "-g", "-O1", "-fno-builtin-malloc", "-fno-builtin-calloc", "-fno-builtin-realloc",
             "-fno-builtin-free", "-Itests"]
    if not IS_MAC:
        flags += ["-D_DEFAULT_SOURCE", "-D_GNU_SOURCE"]
    env_run = {}
    if kind == "tsan":
        cc = os.environ.get("SAN_CC", "/opt/homebrew/opt/llvm/bin/clang" if IS_MAC else "clang")
        if IS_MAC:
            sdk = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True, text=True).stdout.strip()
            flags += ["-isysroot", sdk]
        flags += ["-fsanitize=thread"]
        env_run = {"TSAN_OPTIONS": "halt_on_error=1", "MK_TEST_SCALE": "4"}
    if kind == "guard":
        srcs = ["src/mallockit.c", "tests/guard/t_guard.c", "tests/test_main.c"]
        flags += ["-DMK_DEBUG=1"]
    else:
        srcs = ["src/mallockit.c"] + sorted(
            os.path.join("tests", f) for f in os.listdir(os.path.join(work, "tests")) if f.startswith("t_")) + \
            ["tests/test_main.c"]
    rc, out, _ = sh([cc] + flags + srcs + ["-lpthread", "-o", "t"], work)
    if rc != 0:
        return "build failed", out[-500:], 0.0
    env = dict(os.environ)
    env.update(env_run)
    t0 = time.time()
    p = subprocess.run(["./t"] + ([filt] if filt else []), cwd=work, env=env, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, timeout=900)
    out = p.stdout.decode("utf-8", "replace")
    return ("killed" if p.returncode != 0 else "survived"), out, time.time() - t0


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else ""
    results = []
    base = tempfile.mkdtemp(prefix="mk-mutants-")
    try:
        for name, path, orig, repl, kind, filt, what in MUTANTS:
            if only and only not in name:
                continue
            work = os.path.join(base, name.replace("/", "_"))
            for d in ("src", "include", "tests"):
                shutil.copytree(os.path.join(ROOT, d), os.path.join(work, d))
            fpath = os.path.join(work, path)
            lines = open(fpath).read().split("\n")
            hits = [i for i, l in enumerate(lines) if l.strip() == orig]
            if len(hits) != 1:
                print(f"{name}: expected exactly one line to mutate, found {len(hits)}")
                results.append({"name": name, "status": "not applied"})
                continue
            indent = lines[hits[0]][:len(lines[hits[0]]) - len(lines[hits[0]].lstrip())]
            lines[hits[0]] = indent + repl
            open(fpath, "w").write("\n".join(lines))
            status, out, secs = build_and_test(work, kind, filt)
            failed = [l.split()[1] for l in out.splitlines() if l.startswith("FAIL ")]
            tsan = "ThreadSanitizer: data race" in out
            print(f"{name:30s} {status:9s} {secs:6.1f}s  {', '.join(failed[:4])}{' (TSan: data race)' if tsan else ''}",
                  flush=True)
            results.append({"name": name, "file": path, "original": orig, "mutant": repl, "build": kind,
                            "filter": filt, "simulates": what, "status": status, "failed_tests": failed,
                            "tsan_race": tsan, "seconds": round(secs, 1)})
    finally:
        shutil.rmtree(base, ignore_errors=True)
    os.makedirs(os.path.join(ROOT, "results", "mutants"), exist_ok=True)
    if not only:
        with open(os.path.join(ROOT, "results", "mutants", "mutants.json"), "w") as fh:
            json.dump(results, fh, indent=1)
    killed = sum(r["status"] == "killed" for r in results)
    print(f"{killed}/{len(results)} mutants killed")
    sys.exit(0 if killed == len(results) else 1)


if __name__ == "__main__":
    main()
