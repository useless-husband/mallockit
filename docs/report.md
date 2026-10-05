# mallockit: project report

*A dynamic storage allocator in the style of MIT 6.172 project 3, grown into a thread-caching
`malloc` that replaces the system allocator of real programs on macOS and Linux.*

Every number below comes from the files in `results/` (raw JSON lines) and can be regenerated
with the commands in §9; `python3 analysis/tables.py` prints the tables of this report from them.

## 1. Setup and method

**Machine.** Apple M5 (4 performance + 6 efficiency cores), 16 GB, macOS 27.0.1, 16 KiB pages,
Apple clang 17. The machine was **shared with other jobs** during all measurements. The
1-minute load average recorded before each run was 2 – 3 for the single-thread workloads and
up to 17 during the 8- and 10-thread runs (partly our own threads, partly other jobs); treat
differences under ~10 % as noise, and the 8/10-thread points as indicative only.

**Competitors.** The macOS system allocator (libmalloc), mimalloc 2.2.4 and jemalloc 5.3.0, both
built from source at pinned versions with their default configurations (`bench/fetch.sh`).

**Injection.** Every workload is one unmodified binary; the allocator is chosen with
`DYLD_INSERT_LIBRARIES`. mallockit and mimalloc interpose `malloc` and friends directly and also
register a default zone. jemalloc on macOS only registers a default zone (its default build
there), so each of its calls goes through libmalloc's zone dispatch; this costs it a few
nanoseconds per call and shows in the single-threaded micro-workloads.

**Workloads** (sources from mimalloc-bench, commit 69c41ed7, built with its CMake files):
`cfrac` (factoring; many small short-lived objects), `espresso` (logic minimisation),
`glibc-simple` (single-thread malloc/free loop), `malloc-large` (5 – 25 MiB buffers),
`cache-scratch` (false sharing induced by the allocator), `larson` (server simulation: threads
free objects allocated by their predecessors), `mstress` (mimalloc's stress test: threads
transfer objects and are recreated every iteration; 200 iterations instead of mimalloc-bench's
25 so that a run lasts about a second), `xmalloc-test` (producer threads allocate, consumer
threads free), `glibc-thread` (per-thread malloc/free throughput). `larson`, `xmalloc-test` and
`glibc-thread` run for a fixed 2 s and report throughput; the others are timed.

**Repetitions.** Single-thread workloads 5 times, thread-scaling workloads 3 times for each
thread count (1, 2, 4, 6, 8, 10). Inside each repetition every allocator runs once, in a shuffled
order, so slow drifts of background load do not systematically hit one allocator. Reported:
median; the results page also gives the min – max spread.

**Memory.** Peak *physical footprint* of the process (`proc_pid_rusage`,
`ri_lifetime_max_phys_footprint`, read from the exited child before it is reaped). Not the
resident set size: on macOS the RSS keeps counting pages an allocator returned with
`MADV_FREE_REUSABLE` until the kernel reclaims them, which during development made mallockit
look as if it used twice the memory it really did (§3.6).

## 2. Design in one page

(Full version with the rejected alternatives: [DESIGN.md](DESIGN.md).)

* 4 MiB segments aligned to 4 MiB, so `p & ~(4 MiB − 1)` finds a block's metadata. Small
  segments hold 64 pages of 64 KiB (blocks up to 8 KiB), medium segments 8 pages of 512 KiB
  (up to 64 KiB); larger objects get a segment (or an exact mapping) each.
* 44 size classes: 16-byte steps to 128 B, then four classes per power of two (waste < 20 %).
* Per-thread heaps. Each page keeps a `free` list (allocation), a `local_free` list (the owner's
  frees) and an atomic `xthread_free` stack (other threads' frees). The common `malloc` and
  `free` take no lock and execute no atomic instruction.
* Full pages leave the bin queue; the first remote free into one is redirected to the owner
  heap's `delayed_free` list through a tag in `xthread_free`, so the owner finds it again.
* Heaps are type-stable (never freed): remote frees can always touch the owner heap. On thread
  exit a heap is abandoned; new threads adopt abandoned heaps whole; threads about to map new
  memory first reclaim empty pages from abandoned heaps.
* Memory goes back to the OS after a 10 ms delay (`MADV_DONTNEED` on Linux,
  `MADV_FREE_REUSABLE`/`MADV_FREE_REUSE` on macOS); a 64-entry segment cache keeps at most 64 MiB
  dirty.
* Replacement: `LD_PRELOAD` on Linux; on macOS a default malloc zone plus dyld interposing.

## 3. Optimisations and what each one is worth

Two kinds of evidence: an **ablation** run of the final code (`tools/ablation.py`: one design
decision switched off at a time, six workloads, 3 interleaved repetitions, medians), and
before/after measurements taken while developing (recorded in the commit history).

| variant (what is removed) | espresso | glibc-simple | larson (4 thr) | malloc-large | mstress (4 thr) | xmalloc-test (4 thr) |
|---|---:|---:|---:|---:|---:|---:|
| baseline: time or throughput, peak MiB | 2.13 s, 3 | 0.92 s, 2 | 224 M/s, 35 | 0.56 s, 444 | 0.55 s, 65 | 394 M/s, 34 |
| `no-local-free`: owner frees go straight to `free` | −1 %, 3 | +0 %, 2 | +3 %, 35 | +1 %, 444 | −1 %, 64 | +0 %, 17 |
| `no-keep-last-page`: retire every empty page at once | −4 %, 3 | **−10 %**, 2 | −0 %, 35 | −0 %, 444 | −1 %, 68 | −7 %, 14 |
| `no-adopt`: new threads never adopt exited threads' heaps | +0 %, 3 | +0 %, 2 | −5 %, **775** | −0 %, 444 | **−29 %, 3303** | −5 %, 19 |
| `no-mmap-hint`: always map, unmap, over-map and trim | −0 %, 3 | +1 %, 2 | −1 %, 38 | +1 %, 444 | −0 %, 66 | +5 %, 28 |
| `tiny-cache`: segment cache of one entry | −0 %, 3 | −8 %, 2 | −1 %, 38 | +19 %, 406 | **−25 %**, 54 | −3 %, 17 |
| `tsd-pthread`: heap lookup via `pthread_getspecific()` | −2 %, 3 | −6 %, 2 | −1 %, 35 | +1 %, 444 | −3 %, 65 | −8 %, 13 |
| `purge-now`: purge free pages immediately | −0 %, 3 | −1 %, 2 | −1 %, 35 | **−27 %**, 394 | **−18 %**, 65 | +1 %, 20 |
| `purge-never`: never purge | +0 %, 3 | −0 %, 2 | −1 %, 35 | **+68 %, 599** | −1 %, 67 | −2 %, 21 |

(Positive = faster than the full allocator. Differences of a few percent are noise on this
machine; the peak memory of xmalloc-test varies between 13 and 34 MiB from run to run.)

### 3.1 Thread-local heaps with sharded free lists (the core design)

Not switchable, so measured against the system allocator, whose small-object path is protected
by per-CPU-magazine locks: larson at 4 threads 221 M ops/s vs 15 M (14.8×), glibc-thread at 4
threads 598 M vs 209 M iterations/s (2.9×), and single-threaded glibc-simple 0.92 s vs 1.36 s.

### 3.2 Reading the TSD slot directly (macOS)

On macOS a `__thread` variable in a library is allocated lazily with `malloc` on first access,
which cannot work inside `malloc`, so the heap pointer lives in a pthread key. `pthread_getspecific`
is a function call; mallockit instead reads its slot from the TSD array that `tpidrro_el0` points
to, after checking at start-up that this gives the same answer as `pthread_getspecific`
(otherwise it falls back). Measured: 6 % on glibc-simple, 8 % on xmalloc-test (`tsd-pthread`).

### 3.3 Keeping the last empty page of each size class

A loop that allocates and frees one object would otherwise map, retire and purge a page on every
iteration. Worth 10 % on glibc-simple and 4 – 7 % elsewhere (`no-keep-last-page`).

### 3.4 Adopting the heaps of exited threads

The largest effect in the table, and on memory rather than speed: without it larson's peak
footprint grows 22× (35 → 775 MiB) and mstress's 51× (65 → 3303 MiB, and 29 % slower), because
memory held by dead threads' heaps can only come back through the slower reclaim path.

### 3.5 Segment cache, mmap hint and purge policy

* The cache matters for mstress (25 % slower with a one-entry cache) where segments are released
  and re-acquired as threads come and go; during development, raising it from 16 to 64 entries
  and adding the aligned-address hint cut mstress's `mmap`/`munmap` calls from 4187/4929 to
  450/410 per run. With the cache in place, the hint alone no longer shows in the timings
  (`no-mmap-hint`), because new mappings have become rare.
* The purge delay is a time/memory dial. `purge-never` makes malloc-large 68 % faster at 599 MiB
  instead of 444; `purge-now` costs 27 % (malloc-large) and 18 % (mstress). The default (10 ms,
  at most 64 MiB dirty in the cache) sits between them.
* `tiny-cache` made malloc-large 19 % *faster* with less memory: for 5 – 25 MiB objects,
  unmapping and mapping fresh memory beat caching with purge-and-reuse in that run. An A/B of
  "unmap huge mappings instead of purging them" was inconclusive under the machine's load at the
  time, so the policy was left as it is; it is the most promising lead for the malloc-large gap.

### 3.6 Declaring reuse only for the range that is used (macOS footprint)

Taking a purged segment from the cache first called `MADV_FREE_REUSE` on all 4 MiB of it.
That call charges the whole range back to the process's physical footprint, touched or not.
Declaring reuse page by page when a page is handed out (and for a large object only up to its
size) brought mstress's peak footprint from 110 – 121 MiB to 64 – 66 MiB at the same speed.
This bug was invisible in RSS, which on macOS counts reusable pages anyway (about 130 MiB both
before and after); it only showed once the harness measured the footprint.

### 3.7 Caching huge mappings best-fit

Objects larger than a segment were first unmapped on free. Caching them, reusing the smallest
that fits, and purging only the unused tail (keeping the mapping, so it can serve a larger
request later) took malloc-large from 1.6 s to 0.56 s (together with the fix in §3.6).

### 3.8 One translation unit

`src/mallockit.c` includes every layer. Before, a sample profile of mstress had about a sixth of
the allocator's own samples in the out-of-line ownership check (`mk_owns` → `mk_segmap_test`)
that the interposed `free` runs on every call; the unity build inlines it.

### 3.9 Free-list sharding of owner frees

`no-local-free` (owner frees pushed straight onto `free` instead of a separate `local_free`) shows
no measurable difference on these workloads. mimalloc gives two reasons for the separate list:
the allocation fast path never sees a block that was freed a moment ago (helps temporal
locality of the *program*), and the generic path is guaranteed to run regularly. mallockit keeps
it for the second reason (that is where remote frees are collected and empty pages retired);
the ablation does not show a speed benefit.

### 3.10 What did not help

For the xmalloc-test gap to mimalloc: aligning page descriptors to 128-byte cache lines, and
moving the remote-free stack to its own cache line (to rule out false sharing between the owner's
`free`/`used` fields and other threads' CAS) gave 345 – 362 vs 335 – 397 M frees/s: no change
beyond noise. For mstress at 10 threads: preferring the least-dirty cached segment for large
objects (to avoid purging the tail of a recycled heap segment) did not reduce the 91 000
`madvise` calls per run.

## 4. Verification

| check | what it covers | result |
|---|---|---|
| `./build/test_unit` | 41 tests: size classes (exhaustive over every size ≤ 64 KiB), OS layer, segments and the segment map, pages and free lists, heaps (abandon, adopt, reclaim, fork), the API contract (errno, zeroing, every alignment up to 64 MiB, realloc), the randomised trace driver, four multi-threaded stress tests | 41 passed |
| `./build/test_guard` | 12 tests: each guard-mode detector against one deliberate error (overrun by one byte for ten sizes up to 3 MB, overrun of an aligned block and through `realloc`, local and cross-thread double free, interior, stack and wild pointers, write after free, corrupted free-list link, the default handler's message and `SIGABRT`) | 12 passed |
| `make test-override` | an ordinary C program (libc-internal allocations such as `strdup`, `qsort` data, threads with cross-thread frees, `fork`, the macOS zone path `malloc_zone_malloc(malloc_default_zone())`) and a C++ program (containers, `shared_ptr`, objects deleted on another thread) run with the shared library injected, release and guard builds | 4 program runs ok |
| `make tsan` (Homebrew LLVM) | ThreadSanitizer over all unit and stress tests at full size | 41 passed, no reports |
| `make ubsan` | UndefinedBehaviorSanitizer over unit and guard tests | 41 + 12 passed |
| `make asan` | AddressSanitizer; Linux CI only (ASan hangs on an empty program on this macOS version) | in CI |
| trace driver, 20 seeds | the randomised trace test with seeds 1 – 20 | passed |

**The randomised trace driver** (`tests/t_trace.c`) runs 120 000 operations per seed over 1 500
slots (and a 200 000-operation run over 20 000 slots, and one with the purge delay at 0 so that
purge-then-reuse is hit constantly). Each operation picks `malloc`, `calloc`, `posix_memalign`,
`aligned_alloc` or `realloc(NULL)` for an empty slot, or `realloc`/`free` for a full one, with
sizes from 0 B to 8 MiB and alignments from 8 B to 4 MiB. Every live block is filled with a
pattern derived from its slot; the driver checks alignment and `usable_size` on every call, that
`calloc` memory is zero, that `realloc` preserved the common prefix, that the pattern is intact
before every free, and every 4 096 operations it sorts all live blocks by address and checks that
their *usable* ranges do not overlap. A failure prints the seed.

**The stress tests** end with a leak check: after all threads are joined and `mk_collect(true)`
has run, `segments_in_use` and `large_in_use` must be zero. That turns any lost remote free into
a deterministic failure instead of a slow leak.

**Mutation checks** (`tools/mutants.py`): each mutant changes exactly one line, rebuilds the
tests from a copy of the tree and runs them. All 12 were caught ("killed"). The list shows the
first failing tests; several mutants also crash the test process, which counts as caught.

| mutant | file | simulated bug | caught by |
|---|---|---|---|
| sizeclass-rounds-down | `src/internal.h` | sizes > 128 B get the bin one step too small | api_alignment_contracts (+ crash) |
| remote-free-not-atomic | `src/alloc.c` | remote free pushes with a plain load/store instead of a CAS | stress_producer_consumer, stress_remote_frees_into_full_pages |
| remote-free-not-atomic, TSan build | `src/alloc.c` | same | ThreadSanitizer: data race |
| page-bitmap-off-by-one | `src/segment.c` | marks the neighbouring page as taken, so a page is handed out twice | api_alignment_contracts, api_good_size_matches_usable |
| segment-map-off-by-one | `src/segment.c` | records segments one 4 MiB slot too high in the ownership bitmap | api_owns_rejects_foreign_pointers, api_sizes_are_usable_and_aligned |
| aligned-size-not-rounded | `src/alloc.c` | aligned allocation does not round the size up to the alignment | api_alignment_contracts, segment_huge_alignment_layout |
| collect-forgets-used | `src/page.c` | collecting remote frees does not update the in-use count | page_collect_merges_remote_frees, stress leak checks |
| delayed-free-tag-ignored | `src/alloc.c` | remote frees into a full page never notify the owner heap | page_full_list_and_delayed_free, stress_remote_frees_into_full_pages |
| realloc-copies-half | `src/alloc.c` | realloc copies only half of the old contents | api_realloc_keeps_contents |
| thread-exit-keeps-owner | `src/heap.c` | an exited thread's segments keep its thread id | heap_abandoned_then_adopted |
| guard-canary-unchecked | `src/debug.c` | guard mode stops checking canaries | guard_detects_overrun_by_one_byte (+2) |
| guard-double-free-unchecked | `src/debug.c` | guard mode stops checking the allocation bitmap | guard_detects_double_free (+2) |

**Real programs** (`tools/realprogs.py`). Unmodified binaries with the allocator injected;
"answer" is a digest of each program's output or the test-suite verdict, and it was identical to
the system allocator's for every program and every allocator, including the guard build (which
reported no error in any of them).

| program | what runs | system | mallockit | mallockit (guard) | mimalloc | jemalloc |
|---|---|---|---|---|---|---|
| DuckDB 1.5.6 (Python wheel) | 7 queries over a generated 4 M-row table, 4 threads | 0.8 s, 498 MiB | 0.8 s, 419 MiB | 1.0 s, 433 MiB | 0.8 s, 418 MiB | 0.8 s, 463 MiB |
| sqlite3 3.53 shell (Homebrew) | `bench/sqlite_workload.sql` (600 k rows, index, joins) | 0.3 s, 48 MiB | 0.3 s, 50 MiB | 0.4 s, 51 MiB | 0.3 s, 48 MiB | 0.3 s, 45 MiB |
| CPython 3.12 regression tests | 49 test modules, `PYTHONMALLOC=malloc`, `-j4` | 49 OK, 34 s | 49 OK, 31 s | 49 OK, 38 s | – | – |
| Lua 5.5.0 test suite (Lua 5.5 from Homebrew) | 27 of the 30 test files `all.lua` runs (see notes), one process each | 27/27, 2.0 s | 27/27, 2.1 s | 27/27, 4.1 s | – | – |
| ouro (C compiler) | compiles the SQLite 3.50 amalgamation (9 MB of C) to assembly | 0.4 s | 0.4 s | 0.4 s | 0.4 s | 0.4 s |

Notes. CPython's worker processes inherit the injected allocator; with `PYTHONMALLOC=malloc`
every Python object goes through `malloc` instead of pymalloc. Lua: `main.lua` runs the
interpreter through `/bin/sh`, which drops `DYLD_*` variables, `files.lua` needs `/dev/full`,
which the sandbox here denies, and `big.lua` only works inside `all.lua`'s coroutine driver;
`heavy.lua` is not part of `all.lua`. ouro (the repository owner's self-hosting C compiler,
used read-only) allocates from its own large arenas, so the allocator makes no difference to it;
it is here as a correctness check. Memory figures for CPython and ouro are the main process only.

**Linux** is covered by CI only (this machine has no Linux): GCC and Clang builds, all tests,
`LD_PRELOAD` smoke tests with the C and C++ programs plus python3, perl, sort and git, and ASan,
UBSan and TSan jobs.

**Self-review.** After the measurements, the code was reread for races, leaks and overstated
claims. Found and fixed: guard mode read the would-be segment header of a freed pointer before
checking that the pointer belonged to mallockit, so freeing a wild pointer could crash instead of
being reported (regression test: freeing a pointer into unmapped memory); the smoke test assumed
a macOS 27 detail of `malloc_zone_from_ptr`; and the `glibc-thread` numbers were first recorded
per 2-second run instead of per second.

## 5. Results against the other allocators

Medians; memory = peak physical footprint in MiB (mallockit / system / mimalloc / jemalloc).
The results page shows every thread count with min – max ranges.

| workload | threads | mallockit | macOS system | mimalloc 2.2.4 | jemalloc 5.3.0 | peak MiB |
|---|---:|---:|---:|---:|---:|---:|
| glibc-simple | 1 | **0.92 s** | 1.36 s | 0.94 s | 2.25 s | 2 / 2 / 2 / 2 |
| cfrac | 1 | 1.71 s | 2.01 s | **1.70 s** | 2.68 s | 3 / 2 / 2 / 2 |
| espresso | 1 | **2.15 s** | 2.38 s | 2.18 s | 2.60 s | 3 / 5 / 5 / 4 |
| malloc-large | 1 | 0.56 s | **0.26 s** | 0.26 s | 1.49 s | 444 / 672 / 622 / 414 |
| larson | 1 | **60 M/s** | 21 M/s | 58 M/s | 48 M/s | 13 / 9 / 16 / 11 |
| larson | 4 | **221 M/s** | 15 M/s | 217 M/s | 160 M/s | 42 / 52 / 50 / 37 |
| larson | 10 | **380 M/s** | 36 M/s | **380 M/s** | 255 M/s | 99 / 84 / 92 / 89 |
| glibc-thread | 4 | **598 M/s** | 209 M/s | 568 M/s | 155 M/s | 6 / 3 / 19 / 5 |
| glibc-thread | 10 | **1147 M/s** | 379 M/s | 1015 M/s | 293 M/s | 13 / 6 / 43 / 10 |
| xmalloc-test | 4 | 356 M/s | 264 M/s | **590 M/s** | 205 M/s | 20 / 157 / 35 / 41 |
| xmalloc-test | 10 | 611 M/s | 234 M/s | **710 M/s** | 261 M/s | 61 / 80 / 87 / 99 |
| mstress | 4 | 0.55 s | 0.69 s | **0.52 s** | 0.88 s | 64 / 50 / 81 / 47 |
| mstress | 10 | 2.06 s | 1.89 s | **1.43 s** | 2.51 s | 193 / 164 / 299 / 174 |
| cache-scratch | 10 | **0.10 s** | 0.10 s | 0.11 s | 0.11 s | 2 / 1 / 2 / 2 |

Over all 34 (workload, thread count) cases, the geometric mean of the speed ratios puts
mallockit at **2.08× the system allocator** (faster in 29 cases), **1.76× jemalloc** (31) and
**0.93× mimalloc** (faster in 19 cases, but the losses are larger than the wins).

**Where the system allocator loses.** libmalloc's magazines are per CPU but protected by locks,
and objects freed by another thread go back through them; `larson` (every thread frees its
predecessor's objects) runs at 15 – 36 M ops/s on it against 221 – 380 M with mallockit and
mimalloc. On single-threaded code the difference is smaller (glibc-simple 1.47×, cfrac 1.18×,
espresso 1.11×).

**jemalloc on macOS** pays libmalloc's zone dispatch on every call (it does not interpose), which
is most of its 2.4× deficit on glibc-simple; this is how jemalloc is built for macOS by default,
not a property of jemalloc on Linux.

**Scaling (4 P + 6 E cores).** larson goes 60 → 221 M/s from 1 to 4 threads (3.7×) and only to
380 M/s at 10 threads; glibc-thread 174 → 598 → 1147 M/s. Up to 4 threads macOS can give every
thread a performance core; the next six threads land on efficiency cores, which run this kind of
code about 4 – 6× slower at background priority (table below) and somewhat less slowly at normal
priority, so each extra thread adds roughly a third to a half of a P core. The bend at 4 is the
same for every allocator, i.e. a property of the machine, not of the allocators. The 8- and
10-thread points were also measured while other jobs were running (load average up to 17), so
their absolute values are the least reliable in this report.

| single-thread workload, median s | mallockit P | mallockit E | system P | system E | mimalloc P | mimalloc E |
|---|---:|---:|---:|---:|---:|---:|
| cfrac | 1.71 | 10.82 (×6.3) | 2.01 | 12.56 (×6.3) | 1.70 | 10.81 (×6.4) |
| espresso | 2.15 | 8.87 (×4.1) | 2.38 | 9.76 (×4.1) | 2.18 | 8.66 (×4.0) |
| glibc-simple | 0.92 | 3.71 (×4.0) | 1.36 | 6.29 (×4.6) | 0.94 | 3.47 (×3.7) |

("E" = darwin background priority, which pins a process to the efficiency cores and also lowers
their clock: the worst case for a background thread, not the E cores at full speed.)

**False sharing.** `cache-scratch` (each thread repeatedly writes an object it received from the
main thread) shows no difference between the four allocators: none of them hands objects from
different threads out of the same cache line, and the curve follows the core count.

## 6. The 6.172-style score

6.172's project 3 grades an allocator by space utilisation (peak payload over heap size) and
throughput on traces of `malloc`/`realloc`/`free`. `bench/score.py` generates seven such traces
(deterministic seeds; 25 thousand to 4 million operations and 12 – 60 MiB of peak payload each; the
docstring describes each) and `build/trace_replay` replays them with whatever allocator is
injected: once writing every payload byte and recording the growth of the peak physical
footprint (utilisation U), and three times without touching the payload for throughput (median).
Score = geometric mean over traces of U^0.5 · min(T/T_system, 4)^0.5. The course used its own
traces and weights; this is the same idea, not its grader.

| allocator | score | utilisation (geo-mean) | throughput vs system |
|---|---:|---:|---:|
| mallockit | **1.342** | 0.724 | **2.49×** |
| macOS system | 0.749 | 0.561 | 1.00× |
| mimalloc 2.2.4 | 1.217 | **0.761** | 1.95× |
| jemalloc 5.3.0 | 0.762 | 0.715 | 0.81× |

| trace | U mallockit | U system | U mimalloc | U jemalloc | Mops/s mallockit | system | mimalloc | jemalloc |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| binary | 1.00 | 0.66 | 0.99 | 0.98 | 190 | 102 | 198 | 89 |
| coalesce | 0.67 | 0.22 | 0.51 | 0.27 | 170 | 75 | 114 | 27 |
| random | 0.87 | 0.83 | 0.86 | 0.88 | 99 | 59 | 88 | 77 |
| realloc | 0.51 | 0.43 | 0.83 | 0.77 | 10 | 4 | 4 | 3 |
| strings | 0.84 | 0.83 | 0.85 | 0.83 | 433 | 119 | 411 | 120 |
| phases | 0.56 | 0.53 | 0.64 | 0.87 | 173 | 66 | 133 | 49 |
| trees | 0.75 | 0.75 | 0.75 | 0.74 | 249 | 78 | 256 | 86 |

Reading the table:

* **binary** (free all 448 B blocks, then ask for 512 B blocks) is the classic trap for
  segregated size classes, but mallockit scores 1.00: the 448 B pages become completely empty,
  are retired to their segments, and the 512 B bin takes those same pages. Reuse happens at
  page granularity, not block granularity.
* **coalesce** (free a wave of blocks, request twice the size) works the same way; the system
  allocator keeps the freed regions in per-size free lists and grows instead (0.22).
* **realloc** is mallockit's weak trace (0.51 vs mimalloc's 0.83): buffers grow in 25 % steps
  up to 512 KiB; every medium buffer that outgrows its bin is copied to a new block, and every
  large one sits in its own 4 MiB segment whose dirty tail is kept until it is purged. mimalloc
  places such objects in variable-size spans inside shared segments.
* **phases** (build 60 MB of log-uniform sizes, free 90 % at random, then build again with small
  sizes) is where jemalloc wins (0.87). mallockit can give a page to another size class only once
  *every* block in it is free; after random frees most 512 KiB medium pages still hold a live
  block. jemalloc serves sizes above 16 KiB as individual extents that are freed and coalesced
  one by one, which is the likely reason (not verified further).

Throughput is high for mallockit and mimalloc because both traces' fast paths are a pop and a
push on a thread-local list; T is capped at 4× but no allocator reached the cap.

## 7. Where mallockit loses, and why (as far as measured)

* **malloc-large: 2.2× slower than the system allocator, 34 % less memory.** The workload keeps
  20 buffers of 5 – 25 MiB and replaces one at a time, zero-initialising each. mallockit caches
  freed mappings best-fit but keeps at most 64 MiB of them dirty; beyond that it purges with
  `madvise` and declares reuse again later. A profile shows more samples in `madvise` than in
  `bzero`. The system allocator and mimalloc keep more memory (672 / 622 MiB) and avoid those
  calls. With purging switched off mallockit takes 0.33 s at 599 MiB (ablation `purge-never`),
  so this is mostly a policy choice; the `tiny-cache` result (§3.5) suggests that for mappings
  this large, unmapping would also beat purging.
* **xmalloc-test: 0.6 – 0.86× of mimalloc** (but ahead of the system allocator and jemalloc).
  One thread allocates batches of 4096 objects that another frees. The profile puts the time in
  the remote free's CAS and in the owner's walk of the remote list when it collects it, i.e.
  moving cache lines between cores. Two hypotheses were tested and rejected: false sharing
  between neighbouring page descriptors (aligning them to 128 bytes) and between the owner's
  fields and the remote-free stack (moving it to its own cache line); neither changed the result
  beyond noise (§3.10). mimalloc uses the same data structure, so the difference is in a detail
  not yet found.
* **One producer, one consumer, one object at a time** (the launcher's in-process micro-benchmark
  `build/ubench`, "alloc here, free there"): about 52 ns per object against the system
  allocator's 39 – 54 ns. Every object costs mallockit a CAS on the remote list plus the owner's
  later walk of it; with xmalloc-test's batches of 4096 objects that cost is amortised and
  mallockit is 2.5× faster than the system allocator at 1 thread.
* **mstress at 6 – 10 threads: up to 1.4× slower than mimalloc.** mstress recreates all threads
  every iteration and passes objects between them; at 4 threads and below mallockit is within
  5 % of mimalloc. A profile at 10 threads puts `madvise` at the top of the allocator's samples:
  a run makes about 91 000 `madvise` calls, and still 55 000 with purging disabled. By the code
  paths that remain active in that setting, they most likely come from large objects placed in recycled 4 MiB heap segments (the unused, dirty tail is purged
  right away so that a 100 KiB object does not pin 4 MiB) and from the per-page reuse
  declarations that follow when the segment later serves small objects again. mimalloc places
  such objects in spans inside shared segments and never has to do this. A fix would give large
  objects their own pool of segments; not done.

## 8. Lessons

1. **Measure the right memory number.** For a day of development the macOS resident set size
   said mallockit used 2× the memory of mimalloc on mstress. Most of it was pages already handed
   back with `MADV_FREE_REUSABLE`, which RSS keeps counting. Switching the harness to the physical
   footprint then exposed a real bug hidden behind the wrong metric: declaring reuse
   (`MADV_FREE_REUSE`) for a whole 4 MiB segment charges all of it to the footprint again.
2. **A profile beats intuition.** The first guess for the xmalloc-test gap was false sharing
   between page descriptors; aligning them to 128-byte lines and moving the remote-free stack to
   its own line changed nothing measurable. The profile says the time is in the remote-free CAS
   and the owner's walk of the remote list, i.e. the cache misses inherent to handing blocks
   between cores; why mimalloc pays less for them is still open.
3. **Thread exit is where the hard bugs are.** Making heaps type-stable removed a whole class of
   races (a remote free racing with its owner's exit) at the cost of never returning ~1.3 KiB per
   peak thread. Adoption of whole heaps turned out to matter more for memory than any purge
   policy (ablation `no-adopt`).
4. **Tests need to be tested.** The mutation checks found nothing missing in the unit tests, but
   writing them forced every mutated line to be unique and every leak check to be strict
   (`segments_in_use == 0` after a full collect), and the self-review found a real bug the tests
   did not cover: guard mode read the "segment header" of a wild pointer before checking that the
   pointer was mallockit's. A regression test now frees a pointer into unmapped memory.
5. **Policy is a trade-off, not a bug.** malloc-large is 2× slower than the system allocator
   with a third less memory; the dirty-bytes bound decides which. The report states both numbers
   instead of tuning the bound to win one benchmark.
6. **A shared machine needs interleaving.** Running allocators round-robin inside every
   repetition (instead of one after the other) and recording the load average is what makes a
   10 % difference interpretable at all on a machine where other jobs come and go.

## 9. Reproducing

```sh
make build test                 # library, tests, replacement smoke tests (about a minute)
make tsan ubsan                 # sanitizers (Homebrew LLVM on macOS); `make asan` on Linux
python3 tools/mutants.py        # mutation checks          -> results/mutants/mutants.json
python3 tools/verify.py         # all checks, summarised    -> results/verification.json
make bench                      # fetch competitors + workloads, run -> results/full/bench.jsonl
python3 bench/score.py          # 6.172-style score          -> results/score/score.json
python3 tools/ablation.py       # design switches            -> results/ablation/ablation.jsonl
python3 tools/realprogs.py      # DuckDB, SQLite, ouro, Lua, CPython -> results/realprogs/realprogs.json
python3 analysis/tables.py      # every table of this report
make page                       # docs/results.html
```

`bench/fetch.sh` pins mimalloc v2.2.4 (00d07c4c), jemalloc 5.3.0 (release tarball, SHA-256
checked) and mimalloc-bench 69c41ed7; the full benchmark takes about 35 minutes on this machine,
`realprogs` about 20.
