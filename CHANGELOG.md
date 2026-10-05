# Changelog

## 0.1.0 (2026-10-05)

First release.

- Allocator: 4 MiB segments with small (64 KiB) and medium (512 KiB) pages, one segment per large
  object, 44 size classes with < 20 % internal waste, per-thread heaps with free-list sharding
  (local and remote free lists per page), delayed frees for full pages, type-stable heaps that
  new threads adopt, abandoned-heap reclaim, segment cache with a dirty-bytes bound, delayed
  purge (`MADV_DONTNEED` / `MADV_FREE_REUSABLE`), in-place realloc, every alignment API, fork
  handlers.
- Integration: static library (`mk_*` API), shared library replacing malloc via `LD_PRELOAD`
  (Linux) or a default malloc zone plus dyld interposing (macOS).
- Guard mode library detecting overruns, double frees, invalid frees, writes after free and
  corrupted free lists.
- Tests: per-layer unit tests, randomised trace driver, multi-threaded stress with leak checks,
  guard-mode tests, mutation checks, TSan/UBSan (ASan in Linux CI), real programs.
- Measurement: mimalloc-bench workloads against the system allocator, mimalloc 2.2.4 and
  jemalloc 5.3.0; a 6.172-style utilisation/throughput score; ablation of design decisions.
