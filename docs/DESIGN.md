# mallockit design

mallockit is a general-purpose `malloc` for 64-bit macOS and Linux. Its structure follows
mimalloc (Leijen, Zorn, de Moura, *Mimalloc: Free List Sharding in Action*, 2019) closely; the
code is a from-scratch reimplementation written to learn how such an allocator works and how to
verify and measure one. This document explains the structure, the problems that were hard, and
the alternatives that were considered and rejected. Numbers quoted here come from
[report.md](report.md), where the method is described.

## 1. Layers

```
  malloc(n) / free(p)                         alloc.c    API, fast paths
        |
  per-thread heap: direct[], page queues      heap.c     thread start/exit, abandon/adopt, fork
        |
  page: one size class, three free lists      page.c     extend, collect, full list
        |
  segment: 4 MiB aligned, page descriptors    segment.c  segment map, cache, purge, large objects
        |
  mmap / munmap / madvise                      os.c       page size, clock, options
```

`src/mallockit.c` compiles all layers as one translation unit so the compiler can inline across
them. Before this, a sampling profile of mstress showed about a sixth of the allocator's own
samples in the out-of-line ownership check (`mk_owns` + `mk_segmap_test`) that the interposed
`free` runs on every call.

## 2. Size classes

| sizes | step | bins | worst-case waste |
|---|---|---|---|
| 1 – 128 B | 16 B | 1 – 8 | < 16 B per block |
| 129 B – 8 KiB | four bins per power of two | 9 – 32 (small pages) | < 20 % |
| 8 KiB – 64 KiB | four bins per power of two | 33 – 44 (medium pages) | < 20 % |
| > 64 KiB | multiple of the OS page | one object per segment | < 1 OS page |

Bin of `n > 128`: with `s = n − 1` and `b = ⌊log2 s⌋`, `bin = 9 + 4(b − 7) + ((s >> (b − 2)) & 3)`;
its size is `2^b + (j + 1)·2^(b−2)`. `tests/t_sizeclass.c` checks exhaustively for every size up to
64 KiB that the bin is the smallest one that fits and that waste stays under the bound.

Every block is 16-byte aligned. All bin sizes are multiples of 16, and page block areas start at
a multiple of the kind's largest block (8 KiB for small pages, 64 KiB for medium pages).

**Rejected:** power-of-two classes (up to 50 % waste, the classic buddy-allocator problem);
finer classes (8 per doubling: less waste per block but twice as many partially filled pages per
thread, which costs more than it saves at these sizes); per-object headers with the size
(16 bytes on every allocation, and the header sits right in front of the user's data where
overruns destroy it).

## 3. Segments and pages

A segment is 4 MiB of address space, aligned to 4 MiB, so `p & ~(4 MiB − 1)` finds the segment
of any block without a lookup table. The segment header holds the page descriptors:

| kind | page size | pages | blocks | header (page 0 starts at) |
|---|---|---|---|---|
| small | 64 KiB | 64 | ≤ 8 KiB | 8 KiB |
| medium | 512 KiB | 8 | ≤ 64 KiB | 64 KiB |
| large | whole segment | 1 | one object > 64 KiB | 256 B (or the alignment) |

A page descriptor is 80 bytes; the whole small-segment header (112 + 64 × 80 bytes) fits in the
first 8 KiB, so the metadata of 4 MiB of small objects occupies two cache-line-dense pages of
memory instead of being spread over block headers.

Pages are carved lazily: a fresh page builds its free list 16 KiB (at least 4 blocks) at a
time, so only touched memory becomes resident.

**Objects larger than one segment** get their own mapping (rounded to the OS page, still aligned
to 4 MiB so masking works). **Objects aligned to a segment or more** (e.g. `posix_memalign(16 MiB,
…)`) are the one case where a block starts on a 4 MiB boundary; there the header is placed exactly
one segment *before* the block, and `free` recognises the case because no other block can start
at a segment boundary (the header is there). The alternative, a global radix tree from address to
segment, would cost a dependent load on every `free`.

**The segment map** is one bit per 4 MiB of the 48-bit address space (an 8 MiB zero-initialised
array; only the words that are touched become resident). It answers "is this pointer mallockit's?",
which the macOS zone interface requires (`size()` is called with arbitrary pointers) and which the
replacement `free` uses to send foreign pointers back to the system allocator.

**Rejected:** mimalloc v2's variable-size "slices and spans" inside larger segments (more
flexible for 64 KiB – 2 MiB objects, but much more code; mallockit instead caches whole segments,
see §6) and a separate size-class map per OS page (jemalloc's rtree; a lookup on every free).

## 4. Free-list sharding

Every page has three singly linked free lists:

* `free`: what the allocation fast path pops from;
* `local_free`: blocks freed by the owning thread;
* `xthread_free`: blocks freed by other threads, a lock-free (Treiber) stack.

The fast paths:

```c
void *malloc(size_t n) {                     /* n <= 1024 */
  heap  = thread-local heap;
  page  = heap->direct[(n + 15) / 16];       /* first page of the bin, or an empty dummy */
  block = page->free;
  if (block) { page->free = block->next; page->used++; return block; }
  return malloc_generic(heap, n);            /* refill */
}

void free(void *p) {
  seg  = p & ~(4 MiB - 1);
  page = &seg->pages[(p - seg) >> seg->page_shift];
  if (seg->thread_id == my_thread_id) {      /* local: no atomics */
    push p onto page->local_free;
    if (--page->used == 0 || page->flags) free_slow(page);
  } else {
    CAS-push p onto page->xthread_free;      /* remote */
  }
}
```

Neither takes a lock. `my_thread_id` is the thread's TSD base register (`tpidrro_el0` on Apple
arm64, `fs:0` on x86-64 Linux), so the local/remote test costs one register read and one load.

Only when `free` runs dry does `malloc_generic` merge `xthread_free` (one atomic exchange for
the whole list) and `local_free` into it. Keeping owner frees in a separate list guarantees that
the slow path runs at a regular cadence, which is where remote frees are picked up and pages are
retired; the ablation in report.md measures what it is worth.

### Full pages and the delayed-free list

A page with no free block is moved from its bin queue to the heap's `full` list, so the slow path
does not keep visiting it. But then the owner would never notice when another thread frees into
it. The fix (also mimalloc's) is a two-bit tag in the low bits of `xthread_free`:

* when a page goes to the full list, the owner sets the tag to `USE_DELAYED` with a CAS that also
  checks that no remote free is pending (otherwise the page is not full);
* a remote free that sees `USE_DELAYED` flips the tag back to `NONE` in the same CAS and pushes
  its block onto the *heap's* `delayed_free` list instead of the page's;
* the owner drains `delayed_free` on its next slow path, frees those blocks locally, and moves
  the page back into its bin queue.

So exactly one remote free per full page takes the longer route. The subtle part is lifetime:
the remote thread must read `seg->heap` *before* its CAS publishes the block (afterwards the owner
may free the whole segment), and the heap must still exist when it pushes onto `delayed_free`
even if the owner thread is exiting at that moment. That is guaranteed by the next section.

## 5. Threads: type-stable heaps, abandonment and adoption

Heap structures are never freed. They are carved from 64 KiB chunks and recycled, so any pointer
to a heap stays valid forever (the memory is "type-stable"). This removes a whole class of
use-after-free races between remote frees and thread exit without reference counts.

**Thread exit** (pthread key destructor): the heap is collected (pending frees merged, empty pages
returned, empty segments released), its segments' `thread_id` is set to 0, and the heap is put on
a global *abandoned* list. From then on every free into those segments, including frees by the
thread that will adopt it, takes the atomic remote path, so nothing races on the heap's plain
fields.

**Thread start**: a new thread first tries to *adopt* an abandoned heap, whole: it sets the
heap's and its segments' owner to itself and continues where the dead thread stopped. Blocks the
dead thread left behind become local again. Without adoption the larson benchmark (threads that
hand their objects to successor threads) peaked at 775 MiB instead of 35 MiB, and mstress at
3.2 GiB instead of 65 MiB while running 29 % slower (ablation `no-adopt` in report.md).

**Reclaim**: memory in abandoned heaps that nobody adopts would be stuck. Before mapping a new
segment, a thread takes one abandoned heap off the list (try-lock, so never blocking), collects
it (its remote frees, delayed frees, empty pages, empty segments go to the cache) and puts it
back. `mk_collect()` reclaims all of them.

Thread identity is reused by the OS: a new thread may get a dead thread's TSD address. Clearing
`thread_id` at abandonment is what makes that harmless; the mutation test
`thread-exit-keeps-owner` checks it.

**Rejected:** a global lock around the heap (what most system allocators did in the 1990s:
correct, and serialises every thread); thread caches in front of a shared allocator (tcmalloc,
jemalloc's tcache: very good, but they need a second layer with locks or per-CPU structures and a
policy for flushing caches; the sharded page design needs neither); reference-counted heaps
(an atomic increment on every remote free).

## 6. Getting memory from the OS and giving it back

* **Page size is queried** (`sysconf(_SC_PAGESIZE)`): 16 KiB on Apple Silicon, 4 KiB on most
  Linux machines. Everything that talks to the kernel rounds to it.
* **Aligned mappings**: the first attempt asks for the address just after the previous segment;
  this usually returns an aligned range in one `mmap`. Otherwise map `size + align` and trim both
  ends (three system calls).
* **Segment cache**: empty segments go to a global cache (64 entries, most recently freed reused
  first). Huge mappings are cached too and reused best-fit. Freed segments stay dirty for
  `MALLOCKIT_PURGE_DELAY` ms (default 10), but at most 64 MiB of dirty bytes are kept: beyond
  that the oldest are purged at once.
* **Purge policy for live segments**: a page that becomes empty is retired to its segment and
  purged after the same delay; the purge pass runs at most once per delay per heap, on the
  retire path. The last empty page of each bin is kept (not retired) so that a loop that
  allocates and frees one object does not map and purge a page every iteration.
* **Purge mechanism**: Linux `MADV_DONTNEED` (memory is released at once and reads as zero).
  macOS `MADV_FREE_REUSABLE`, which takes the pages out of the process's *footprint* at once, plus
  `MADV_FREE_REUSE` before reuse, as Apple's own allocator does. An early version declared reuse
  for whole 4 MiB segments when it took one from the cache; `MADV_FREE_REUSE` charges the whole
  range back to the footprint, touched or not, which doubled mstress's footprint (121 → 65 MiB
  after declaring reuse page by page).

A consequence for measurement: on macOS the resident set size (`ru_maxrss`) still counts
reusable pages until the kernel takes them, so all memory numbers in this project on macOS are
peak *physical footprint* (`proc_pid_rusage`, `ri_lifetime_max_phys_footprint`), the number
Activity Monitor shows.

**Not done:** purging on a timer when a thread goes idle (an idle thread keeps up to its free
pages dirty until its next slow path), and `mremap` for growing huge objects on Linux (the
in-place growth path maps the next address range with a hint and falls back to copying).

## 7. Alignment

`posix_memalign`, `aligned_alloc`, `memalign`, `valloc`, `pvalloc` all go through one rule: for
alignment `A ≤ 64 KiB`, allocate `round_up(n, A)` from the normal bins. The bins are closed under
this rounding (if `n` is a multiple of `A`, so is its bin size; tested exhaustively) and block
areas start at a multiple of the kind's largest block, so every block of such a bin is
`A`-aligned. There are no headers and no interior pointers, so `free` needs no special case.
Larger alignments go to the large-object path, which places the object at an aligned offset of
its segment; alignments of 4 MiB or more use the header-before-block layout of §3.

## 8. realloc

* small/medium: stays in place if the new size fits the block and uses at least half of it;
* large: grows or shrinks inside its mapping by changing the usable size (purging the tail on a
  big shrink); beyond the mapping it tries to map the adjacent range (`mmap` with a hint, never
  `MAP_FIXED`, which would silently replace whatever is there) and otherwise moves.

## 9. Replacing the system allocator

**Linux:** the shared library defines `malloc`, `free`, `calloc`, `realloc`, `reallocarray`,
`posix_memalign`, `aligned_alloc`, `memalign`, `valloc`, `pvalloc`, `malloc_usable_size`.
With `LD_PRELOAD` (or by linking it first) these take precedence for the whole process,
including glibc's own internal calls (`strdup`, `fopen`) and libstdc++'s `operator new`; glibc
documents this as supported. A pointer the segment map does not know goes to `__libc_free`.

**macOS** offers two mechanisms and mallockit uses both, as mimalloc does:

1. **A malloc zone** registered from a library constructor and moved to the front of the zone
   list (unregister and re-register the old default until ours is first; libmalloc reports
   it through its "DefaultMallocZone" wrapper). This catches `malloc_zone_malloc(malloc_default_zone(),
   …)` used by CoreFoundation, Objective-C and Swift, and gives the system a way to route
   `free()` of mallockit pointers through `size()`.
2. **dyld interposing** (`__DATA,__interpose`) of `malloc`, `free`, `calloc`, `realloc`,
   `reallocf`, `valloc`, `posix_memalign`, `aligned_alloc`, `malloc_size`, `malloc_good_size`,
   and the typed-allocation entry points `malloc_type_*` that recent compilers emit. This skips
   the zone dispatch on every call.

What macOS prevents: `DYLD_INSERT_LIBRARIES` is ignored for SIP-protected binaries (everything in
`/usr/bin`, `/bin`, `/System`) and for binaries signed with the hardened runtime unless they carry
the `allow-dyld-environment-variables` entitlement. Homebrew's binaries (ad-hoc signed) and
anything you build yourself work. A shell or `make` started from `/bin` strips `DYLD_*` from its
own environment, so the variable must be set on the command that runs the target program itself.
Linking the static library into a program does **not** replace the allocator on macOS (two-level
namespace: other libraries keep calling libSystem's `malloc`); on macOS use the shared library.
`leaks(1)` and `heap(1)` see no mallockit blocks: the zone's enumerator is not implemented.

## 10. Fork

`pthread_atfork` handlers (or, when registered as a zone, the zone's `force_lock`,
`force_unlock` and `reinit_lock` hooks that libmalloc calls around `fork`) take the two global
locks (heap registry and segment cache) before `fork` and release or re-initialise them after.
In the child only the forking thread exists: heaps owned by other threads are marked ownerless
(their segments' `thread_id` cleared, through a validated walk because a thread may have been
changing its segment list when `fork` happened) and their memory stays allocated; frees into
them take the remote path. Tested by `heap_fork_child_keeps_working` (fork while another thread
holds blocks; the child frees them, allocates, and starts a thread).

## 11. Guard mode

`libmallockit-debug` (built with `MK_DEBUG=1`) adds, per block: a canary after the requested
bytes and the requested size (XOR a magic word) in the last 8 bytes; per page: one bit per block
("handed out"); per free block: a `0xDF` fill. It reports overruns, double frees (also across
threads, through an atomic `fetch_and` on the bit), frees of pointers that are not block starts,
writes after free (checked when the block is handed out again) and corrupted free-list links
(checked before the link is followed). `malloc_usable_size` reports the requested size so that
correct programs cannot write into the canary. Each detector has a test that makes exactly that
error.

## 12. Known limitations

* Memory freed by a thread that then stays idle is purged only on that thread's next slow path.
* A full page is revisited by its owner only through the delayed-free list; a page in the middle
  of a bin queue that collects many remote frees is merged when the owner walks to it.
* Objects of 64 KiB – 4 MiB take a whole (cached) segment each. Address space is not an issue
  on 64-bit systems, but when such an object lands in a recycled, fully dirty segment its unused
  tail is purged at once, and `malloc-large`-style workloads spend time in `madvise` when the
  cache's dirty-byte bound forces purging (report.md, §7).
* Double frees of large objects are detected in guard mode only while their segment is cached.
* Thread exit relies on pthread key destructors. If another library's destructor allocates after
  the last destructor round (POSIX allows 4), that thread's new heap is never abandoned and its
  segments keep a dead thread's id.
* 32-bit platforms and Windows are not supported.
