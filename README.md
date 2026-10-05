# mallockit

**A thread-caching `malloc` written from scratch in C, measured against the macOS system allocator,
mimalloc and jemalloc, and verified with randomised traces, ThreadSanitizer, a guard mode and
mutation checks.**

[繁體中文說明](README.zh-TW.md) · [Design](docs/DESIGN.md) · [Report](docs/report.md) ·
[Results page](docs/results.html) · [導讀（初學者）](docs/導讀.zh-TW.md)

mallockit implements the whole C allocation API (`malloc`, `free`, `calloc`, `realloc`,
`posix_memalign`, `aligned_alloc`, `memalign`, `valloc`, `pvalloc`, `malloc_usable_size` /
`malloc_size`) for 64-bit macOS and Linux, and can replace the system allocator of unmodified
programs: `LD_PRELOAD` on Linux, a default malloc zone plus dyld interposing on macOS. It started
from the dynamic storage allocator project of MIT 6.172 *Performance Engineering of Software
Systems* (scored on space utilisation and throughput) and grew into something that runs DuckDB,
SQLite, CPython's test suite and the Lua test suite. The design follows mimalloc closely; this is
a learning reimplementation, not a new allocator design (see [Related work](#related-work)).

## Results

Apple M5 (4 performance + 6 efficiency cores), macOS 27, measured on a **shared machine**;
medians of 3 – 5 interleaved runs; every allocator runs the same unmodified binary
(`DYLD_INSERT_LIBRARIES`). Full tables, charts and ranges: [docs/results.html](docs/results.html).

| workload (mimalloc-bench) | threads | mallockit | macOS system | mimalloc 2.2.4 | jemalloc 5.3.0 |
|---|---:|---:|---:|---:|---:|
| glibc-simple (time) | 1 | **0.92 s** | 1.36 s | 0.94 s | 2.25 s |
| cfrac (time) | 1 | 1.71 s | 2.01 s | **1.70 s** | 2.68 s |
| espresso (time) | 1 | **2.15 s** | 2.38 s | 2.18 s | 2.60 s |
| larson (ops/s) | 4 | **221 M** | 15 M | 217 M | 160 M |
| larson (ops/s) | 10 | **380 M** | 36 M | **380 M** | 255 M |
| glibc-thread (ops/s) | 10 | **1147 M** | 379 M | 1015 M | 293 M |
| xmalloc-test (frees/s) | 4 | 356 M | 264 M | **590 M** | 205 M |
| mstress (time) | 10 | 2.06 s | 1.89 s | **1.43 s** | 2.51 s |
| malloc-large (time) | 1 | 0.56 s | **0.26 s** | 0.26 s | 1.49 s |
| malloc-large (peak MiB) | 1 | 444 | 672 | 622 | **414** |

Geometric mean of the speed ratios over all 34 (workload, thread count) cases: mallockit runs at
**2.08× the system allocator**, **1.76× jemalloc** and **0.93× mimalloc**. It loses clearly on
`malloc-large` (a deliberate memory/speed trade-off), on `xmalloc-test` against mimalloc, and on
`mstress` above 4 threads; the [report](docs/report.md) analyses each loss. jemalloc on macOS
runs as a malloc zone, so it pays libmalloc's dispatch on every call.

**6.172-style score** (seven generated traces; utilisation = peak payload / peak footprint growth,
throughput relative to the system allocator, score = geometric mean of U^0.5 · T^0.5):

| | mallockit | system | mimalloc | jemalloc |
|---|---:|---:|---:|---:|
| score | **1.34** | 0.75 | 1.22 | 0.76 |
| utilisation (geo-mean) | 0.72 | 0.56 | **0.76** | 0.72 |
| throughput vs system | **2.49×** | 1.00× | 1.95× | 0.81× |


## How it works

```
 malloc(n ≤ 1 KiB)                                   free(p)
   heap = this thread's heap (TSD slot)                seg  = p & ~(4 MiB - 1)
   page = heap->direct[(n+15)/16]                      page = seg->pages[(p - seg) >> shift]
   pop page->free            -> done (no lock)         seg->thread_id == me ? push page->local_free
   else: merge remote + local frees, or a new page                         : CAS-push page->xthread_free

 4 MiB segment (aligned to 4 MiB)
 +----------+---------+---------+-----+---------+
 | header + | page 1  | page 2  | ... | page 63 |   small: 64 KiB pages, blocks 16 B - 8 KiB
 | page 0   |         |         |     |         |   medium: 512 KiB pages, blocks 8 - 64 KiB
 +----------+---------+---------+-----+---------+   large: one object per segment (cached, best fit)
```

* **44 size classes**, four per power of two above 128 bytes: internal waste below 20 %.
* **Free-list sharding**: each page has a free list for allocation, a local list for the owner's
  frees and a lock-free list for other threads' frees. The common malloc and free take no lock and
  no atomic instruction.
* **Full pages and delayed frees**: pages without a free block leave the bin queue; the first
  remote free into such a page is routed to the owner heap through a tag in the remote list, so
  the owner finds it again.
* **Type-stable heaps**: heaps are never freed, which makes remote frees safe while a thread
  exits. A new thread adopts the whole heap of an exited thread; others reclaim empty pages of
  abandoned heaps before asking the OS for memory.
* **Back to the OS**: free pages and empty segments are purged after 10 ms (`MADV_DONTNEED`,
  `MADV_FREE_REUSABLE` on macOS), with at most 64 MiB of dirty memory cached.
* **Alignment** without headers: bins are closed under rounding to any power of two up to 64 KiB,
  so aligned blocks come from the normal size classes.
* **Guard mode** (`libmallockit-debug`): canaries, per-block allocation bits and free-fill detect
  overruns, double frees (also across threads), invalid frees, writes after free and corrupted
  free lists.

[docs/DESIGN.md](docs/DESIGN.md) explains each part and the alternatives that were rejected.

## Verification

<!-- VERIFICATION -->

## Using it

```sh
make build                       # build/libmallockit.{a,so|dylib}, build/libmallockit-debug.*
make test                        # unit + trace + stress, guard mode, replacement smoke tests

# Linux: any dynamically linked program
LD_PRELOAD=$PWD/build/libmallockit.so python3 my_script.py

# macOS: programs that are not SIP-protected and not hardened (Homebrew's, your own builds)
DYLD_INSERT_LIBRARIES=$PWD/build/libmallockit.dylib /opt/homebrew/bin/python3.12 my_script.py

# statistics at exit, purge delay, guard mode
MALLOCKIT_STATS=1 MALLOCKIT_PURGE_DELAY=10 DYLD_INSERT_LIBRARIES=$PWD/build/libmallockit-debug.dylib ./prog
```

As a library, link `build/libmallockit.a` and call `mk_malloc` / `mk_free` / … from
[`include/mallockit.h`](include/mallockit.h) (also `mk_collect`, `mk_stats_get`, and in the guard
build `mk_set_error_handler`). Double-click `跑跑看.command` on a Mac for a guided build, test and
short benchmark.

Other targets: `make tsan` / `ubsan` / `asan` (sanitizers; ASan only on Linux, it hangs on this
macOS version), `make mutants`, `make bench` (fetches mimalloc, jemalloc and mimalloc-bench at
pinned commits into `build/ext`), `make score`, `make ablation`, `make realprogs`, `make page`.

## Limitations

* macOS ignores `DYLD_INSERT_LIBRARIES` for SIP-protected and hardened-runtime binaries, and a
  statically linked mallockit does not replace the allocator of other libraries on macOS
  (two-level namespace). `leaks(1)`/`heap(1)` cannot enumerate mallockit's blocks.
* Losses, measured: `malloc-large` (repeated 5 – 25 MiB buffers) and `xmalloc-test`
  (one thread allocates, another frees); see the report for numbers and analysis.
* An idle thread's free pages are purged only on its next slow path; there is no background
  purge thread.
* Objects between 64 KiB and 4 MiB each take a (cached) 4 MiB segment of address space.
* 64-bit macOS (arm64, x86-64) and Linux (x86-64, arm64) with glibc only; no Windows, no 32-bit.

## Related work

* **MIT 6.172 project 3** (dynamic storage allocator): the starting point; students implement
  `malloc`/`free`/`realloc` on a simulated `sbrk` heap and are graded on utilisation and throughput
  over traces. mallockit's `bench/score.py` reuses that idea with its own traces and a real OS.
* **mimalloc** (Leijen, Zorn, de Moura, 2019): free-list sharding, local/thread-free lists,
  delayed frees, segments of pages, macOS zone + interposing. mallockit follows this design.
  Differences: mallockit adopts whole heaps of exited threads (mimalloc abandons and reclaims
  segments one by one), handles 64 KiB+ objects with whole cached segments instead of spans in
  arenas, and has none of mimalloc's security features, heap API or platform breadth.
* **jemalloc** (Evans, 2006; used by FreeBSD and Facebook): arenas, per-thread caches, extents and
  decay-based purging. **tcmalloc** (Google): thread or per-CPU caches over central free lists.
* **snmalloc** (Liétar et al., 2019): remote frees batched as messages between allocators.
* **Hoard** (Berger et al., 2000): per-processor heaps with superblocks and bounded blowup; the
  source of the false-sharing benchmarks (`cache-scratch`, `cache-thrash`).
* **mimalloc-bench**: the workload collection used here.

## Layout

| Path | Contents |
|---|---|
| `src/` | `os.c`, `segment.c`, `page.c`, `heap.c`, `alloc.c`, `debug.c`, `override.c`; `mallockit.c` is the unity build |
| `tests/` | per-layer unit tests, randomised trace driver, stress tests, guard tests, replacement smoke programs |
| `bench/` | harness (`run.py`), 6.172-style score (`score.py`, `trace_replay.c`), fetch script, workloads |
| `tools/` | mutation checks, ablation study, real programs, verification summary |
| `analysis/page.py` | renders `docs/results.html` from `results/` |
| `results/` | raw measurements (JSON lines) behind every number in the docs |

## License

MIT, see [LICENSE](LICENSE).
