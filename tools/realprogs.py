#!/usr/bin/env python3
"""Run real programs on top of mallockit and compare them with the system
allocator (and, for time and memory, with mimalloc and jemalloc).

Programs (all unmodified binaries; the allocator is injected):
  duckdb   DuckDB (pip wheel in a venv) running bench/duckdb_workload.py
  sqlite   the sqlite3 shell running bench/sqlite_workload.sql
  ouro     a C compiler (the owner's project "ouro", if present next to this
           repository) compiling the SQLite amalgamation to assembly
  lua      the Lua 5.5 interpreter running the official Lua 5.5.0 test suite
  cpython  CPython 3.12's own regression tests (a fixed list of modules)
           with PYTHONMALLOC=malloc so every object goes through malloc

Each result is checked for identical output across allocators (digest of
the program's answer). Results: results/realprogs/realprogs.json.

  python3 tools/realprogs.py --ext build/ext --work build/realprogs
"""
import argparse
import glob
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "bench"))
import run as bench  # noqa: E402

PY312 = "/opt/homebrew/opt/python@3.12/bin/python3.12"
SQLITE = "/opt/homebrew/opt/sqlite/bin/sqlite3"
LUA = "/opt/homebrew/opt/lua/bin/lua"
DUCKDB_VERSION = "1.5.6"
LUA_TESTS = "https://www.lua.org/tests/lua-5.5.0-tests.tar.gz"
CPYTHON_TESTS = ("test_dict test_list test_set test_tuple test_deque test_json test_re test_unicode test_userstring "
                 "test_bytes test_collections test_itertools test_functools test_sort test_heapq test_bisect "
                 "test_array test_struct test_pickle test_threading test_queue test_sqlite3 test_zlib "
                 "test_decimal test_fractions test_statistics test_math test_float test_long test_weakref "
                 "test_gc test_memoryview test_xml_etree test_csv test_io test_subprocess test_enum "
                 "test_dataclasses test_typing test_ast test_compile test_grammar test_string_literals "
                 "test_textwrap test_difflib test_hashlib test_base64 test_binascii test_codecs").split()


def digest(text):
    return hashlib.sha256(text.encode()).hexdigest()[:16]


def ensure_duckdb(work):
    venv = os.path.join(work, "venv")
    py = os.path.join(venv, "bin", "python")
    if not os.path.exists(py):
        subprocess.run([PY312, "-m", "venv", venv], check=True)
        subprocess.run([py, "-m", "pip", "install", "-q", f"duckdb=={DUCKDB_VERSION}"], check=True)
    return py


def ensure_lua_tests(work):
    d = os.path.join(work, "lua-5.5.0-tests")
    if not os.path.isdir(d):
        tgz = os.path.join(work, "lua-tests.tar.gz")
        urllib.request.urlretrieve(LUA_TESTS, tgz)
        with tarfile.open(tgz) as t:
            t.extractall(work)
    return d


def ensure_ouro(work):
    """Copy the compiler binary and the SQLite amalgamation (read-only use of
    the sibling project) into the work directory."""
    cands = glob.glob(os.path.join(os.path.dirname(ROOT), "*ouro"))
    if not cands:
        return None
    src = cands[0]
    binp = os.path.join(src, "build", "ouro")
    amal = glob.glob(os.path.join(src, "build", "sqlite", "sqlite-amalgamation-*", "sqlite3.c"))
    if not os.path.exists(binp) or not amal:
        return None
    d = os.path.join(work, "ouro")
    os.makedirs(d, exist_ok=True)
    shutil.copy2(binp, os.path.join(d, "ouro"))
    shutil.copy2(amal[0], os.path.join(d, "sqlite3.c"))
    inc = os.path.join(d, "include")
    if not os.path.isdir(inc):
        shutil.copytree(os.path.join(src, "include"), inc)
    return d


def programs(work, ext):
    progs = []
    py = ensure_duckdb(work)
    progs.append(("duckdb", [py, os.path.join(ROOT, "bench", "duckdb_workload.py"), "4"], None, {},
                  lambda out: json.loads(out.strip().splitlines()[-1])["digest"]))
    progs.append(("sqlite", ["/bin/sh", "-c", ""], None, {}, None))  # replaced below (stdin redirect)
    od = ensure_ouro(work)
    if od:
        flags = ["-w", "-DSQLITE_THREADSAFE=0", "-DSQLITE_OMIT_LOAD_EXTENSION", "-DSQLITE_WITHOUT_ZONEMALLOC",
                 "-DSQLITE_ENABLE_LOCKING_STYLE=0", "-DSQLITE_ENABLE_FTS5"]  # as ouro's scripts/sqlite.sh
        progs.append(("ouro", [os.path.join(od, "ouro"), "-S", "-O1"] + flags + ["sqlite3.c", "-o", "sqlite3.s"], od,
                      {"OURO_INCLUDE": os.path.join(od, "include")},
                      lambda out, od=od: hashlib.sha256(open(os.path.join(od, "sqlite3.s"), "rb").read())
                      .hexdigest()[:16]))
    ld = ensure_lua_tests(work)
    # The files the suite's driver all.lua runs, each in its own process, in "user" mode (_U: no
    # internal C test library). Skipped: main.lua (runs the interpreter through /bin/sh, which
    # drops DYLD_* variables), files.lua (needs /dev/full, which the sandbox here denies) and
    # big.lua (written to run as a coroutine inside all.lua). heavy.lua is not part of all.lua
    # (it allocates tens of GB) and is not run.
    referenced = set(re.findall(r"([a-z0-9]+)\.lua", open(os.path.join(ld, "all.lua")).read()))
    files = sorted(f + ".lua" for f in referenced if f not in ("all", "main", "files", "big"))
    progs.append(("lua", [[LUA, "-e", "_U=true _port=true", f] for f in files], ld, {}, None))
    progs.append(("cpython", [PY312, "-m", "test", "-j4", "--timeout", "600"] + CPYTHON_TESTS, work,
                  {"PYTHONMALLOC": "malloc"}, None))
    return progs


def sqlite_argv(work):
    # sqlite3 reads the script from stdin; avoid /bin/sh (SIP strips DYLD_*)
    return [SQLITE, ":memory:", ".read '" + os.path.join(ROOT, "bench", "sqlite_workload.sql") + "'"]


def summarize_cpython(out):
    m = re.search(r"Result: (\w+)", out)
    ok = re.search(r"(\d+) tests? OK", out)
    failed = re.search(r"(\d+) tests? failed:?\s*\n((?:\s+.*\n)+)", out)
    skipped = re.search(r"(\d+) tests? skipped", out)
    return {"result": m.group(1) if m else None, "ok": int(ok.group(1)) if ok else 0,
            "failed": failed.group(2).split() if failed else [], "skipped": int(skipped.group(1)) if skipped else 0}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ext", default=os.path.join(ROOT, "build", "ext"))
    ap.add_argument("--work", default=os.path.join(ROOT, "build", "realprogs"))
    ap.add_argument("--out", default=os.path.join(ROOT, "results", "realprogs"))
    ap.add_argument("--only", default="")
    ap.add_argument("--reps", type=int, default=3)
    args = ap.parse_args()
    os.makedirs(args.work, exist_ok=True)
    allocs = bench.allocators(args.ext)
    allocs["mallockit-debug"] = os.path.join(ROOT, "build", "libmallockit-debug." + bench.SO)
    results = []
    for name, argv, cwd, env, check in programs(args.work, args.ext):
        if args.only and name not in args.only.split(","):
            continue
        if name == "sqlite":
            argv = sqlite_argv(args.work)
            check = digest
        heavy = name in ("cpython", "lua")
        names = ["system", "mallockit", "mallockit-debug"] + ([] if heavy else ["mimalloc", "jemalloc"])
        reps = 1 if heavy else args.reps
        for alloc in names:
            for rep in range(reps if alloc != "mallockit-debug" else 1):
                if isinstance(argv[0], list):  # several processes, one per test file
                    parts = [(a[-1], bench.run_measured(a, allocs[alloc], extra_env=env, cwd=cwd or args.work,
                                                        timeout=1800)) for a in argv]
                    bad = [f for f, p in parts if not p["ok"]]
                    r = {"ok": not bad, "wall": sum(p.get("wall") or 0 for f, p in parts),
                         "footprint": max(p.get("footprint") or 0 for f, p in parts),
                         "maxrss": max(p.get("maxrss") or 0 for f, p in parts),
                         "out": f"{len(parts) - len(bad)}/{len(parts)} files OK" + (" failing: " + " ".join(bad) if bad else "")}
                    check = (lambda out: out)
                else:
                    r = bench.run_measured(argv, allocs[alloc], extra_env=env, cwd=cwd or args.work, timeout=1800)
                out = r.get("out", "")
                rec = {"program": name, "alloc": alloc, "rep": rep, "ok": r["ok"], "wall": r.get("wall"),
                       "footprint": r.get("footprint"), "maxrss": r.get("maxrss")}
                if name == "cpython":
                    rec["cpython"] = summarize_cpython(out)
                    rec["answer"] = rec["cpython"]["result"]
                elif check:
                    try:
                        rec["answer"] = check(out)
                    except Exception as e:  # noqa: BLE001
                        rec["answer"] = None
                        rec["error"] = str(e)
                if not r["ok"]:
                    rec["tail"] = out[-1500:]
                if "mallockit: " in out:
                    rec["guard_report"] = [l for l in out.splitlines() if l.startswith("mallockit: ")][:5]
                results.append(rec)
                print(f"{name:8s} {alloc:16s} rep {rep}: ok={r['ok']} {r.get('wall', 0):7.2f}s "
                      f"mem {(r.get('footprint') or 0) >> 20} MiB answer={rec.get('answer')}"
                      + (f" {rec['cpython']}" if name == "cpython" else ""), flush=True)
    os.makedirs(args.out, exist_ok=True)
    path = os.path.join(args.out, "realprogs.json")
    old = []
    if args.only and os.path.exists(path):
        old = [r for r in json.load(open(path))["runs"] if r["program"] not in args.only.split(",")]
    with open(path, "w") as fh:
        json.dump({"machine": bench.machine(), "duckdb": DUCKDB_VERSION, "lua_tests": LUA_TESTS,
                   "cpython_tests": CPYTHON_TESTS, "runs": old + results}, fh, indent=1)
    print("wrote", path)


if __name__ == "__main__":
    main()
