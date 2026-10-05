#!/usr/bin/env python3
"""Run every correctness check and record the outcome in
results/verification.json (shown on the results page).

  python3 tools/verify.py            # all checks (a few minutes)
"""
import json
import os
import platform
import re
import subprocess
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
IS_MAC = platform.system() == "Darwin"


def run(cmd, env=None):
    e = dict(os.environ)
    if env:
        e.update(env)
    t0 = time.time()
    p = subprocess.run(cmd, cwd=ROOT, shell=True, env=e, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode("utf-8", "replace"), time.time() - t0


def counted(out):
    m = re.findall(r"(\d+) passed, (\d+) failed", out)
    return " + ".join(f"{a} passed, {b} failed" for a, b in m) if m else "no summary"


def main():
    checks = []

    def add(name, command, env=None, summary=counted):
        rc, out, secs = run(command, env)
        res = summary(out) if rc == 0 or summary is counted else out.strip().splitlines()[-1:]
        if isinstance(res, list):
            res = " ".join(res)
        checks.append({"name": name, "command": (" ".join(f"{k}={v}" for k, v in (env or {}).items()) + " "
                                                 + command).strip(),
                       "passed": rc == 0, "result": res + f" ({secs:.0f} s)"})
        print(f"{'PASS' if rc == 0 else 'FAIL'} {name}: {res}", flush=True)

    run("make -j4 build")
    add("unit, trace and stress tests", "./build/test_unit")
    add("guard mode tests", "./build/test_guard")
    add("malloc replacement smoke tests", "make -s test-override",
        summary=lambda o: f"{len(re.findall(r'smoke(_cxx)?: ok', o))} programs ok")
    add("ThreadSanitizer, full size", "make -s tsan", env={"MK_TEST_SCALE": "1"})
    add("UndefinedBehaviorSanitizer (unit + guard)", "make -s ubsan")
    if not IS_MAC:
        add("AddressSanitizer", "make -s asan", env={"ASAN_OPTIONS": "detect_leaks=0"})
    add("trace driver, 20 more seeds", "./build/test_unit trace_random_seeds", env={"MK_TRACE_SEEDS": "20"})
    os.makedirs(os.path.join(ROOT, "results"), exist_ok=True)
    with open(os.path.join(ROOT, "results", "verification.json"), "w") as fh:
        json.dump({"platform": platform.platform(), "date": time.strftime("%Y-%m-%d"), "checks": checks}, fh,
                  indent=1)
    print("wrote results/verification.json")


if __name__ == "__main__":
    main()
