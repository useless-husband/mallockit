/* Internal definitions shared by every layer. See docs/DESIGN.md.
 *
 * Layers, bottom to top:
 *   os.c       mmap/munmap/madvise, page size, clock, options
 *   segment.c  4 MiB aligned segments, the segment map, the segment cache,
 *              page purge policy, large objects
 *   page.c     one size class per page: free lists, extension, collection
 *   heap.c     per-thread heaps, thread exit/abandon/adopt, fork
 *   alloc.c    the public API and its fast paths
 *   debug.c    guard mode (only does work when MK_DEBUG=1) */
#ifndef MK_INTERNAL_H
#define MK_INTERNAL_H

#if !defined(__APPLE__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif
#if !defined(__APPLE__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "../include/mallockit.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef MK_DEBUG
#define MK_DEBUG 0
#endif
#ifndef MK_OVERRIDE
#define MK_OVERRIDE 0
#endif
/* Design switches, all on by default. tools/ablation.py builds variants
 * with one of them turned off to measure what it is worth. */
#ifndef MK_KEEP_LAST_PAGE /* keep the last empty page of a bin instead of retiring it */
#define MK_KEEP_LAST_PAGE 1
#endif
#ifndef MK_ADOPT /* new threads adopt the heaps of exited threads */
#define MK_ADOPT 1
#endif
#ifndef MK_MMAP_HINT /* ask for the next aligned mapping right after the last one */
#define MK_MMAP_HINT 1
#endif
#ifndef MK_LOCAL_FREE /* owner frees go to a separate local_free list */
#define MK_LOCAL_FREE 1
#endif

#define mk_likely(x) __builtin_expect(!!(x), 1)
#define mk_unlikely(x) __builtin_expect(!!(x), 0)
#define mk_internal __attribute__((visibility("hidden")))
#define mk_noinline __attribute__((noinline))
#define mk_cold __attribute__((cold, noinline))

/* ---------------------------------------------------------------- sizes */

#define MK_SEGMENT_SHIFT 22 /* 4 MiB segments, aligned to their size */
#define MK_SEGMENT_SIZE ((size_t)1 << MK_SEGMENT_SHIFT)
#define MK_SEGMENT_MASK (MK_SEGMENT_SIZE - 1)

#define MK_SMALL_PAGE_SHIFT 16  /* 64 KiB pages: blocks up to 8 KiB */
#define MK_MEDIUM_PAGE_SHIFT 19 /* 512 KiB pages: blocks up to 64 KiB */
#define MK_SMALL_PAGES (MK_SEGMENT_SIZE >> MK_SMALL_PAGE_SHIFT)   /* 64 */
#define MK_MEDIUM_PAGES (MK_SEGMENT_SIZE >> MK_MEDIUM_PAGE_SHIFT) /* 8 */
#define MK_LARGE_PAGE_SHIFT 63 /* every pointer of a large segment -> page 0 */

#define MK_MIN_ALIGN 16 /* every block is at least 16-byte aligned */
#define MK_SMALL_MAX 8192
#define MK_MEDIUM_MAX 65536
#define MK_DIRECT_MAX 1024 /* sizes served through heap->direct[] */
#define MK_DIRECT_SLOTS (MK_DIRECT_MAX / 16 + 1)

/* Bins: 1..8 are 16..128 in steps of 16; above that four bins per power of
 * two, so the internal waste of a bin is below 20%. Bin 32 = 8 KiB is the
 * largest small bin, bin 44 = 64 KiB the largest medium bin. */
#define MK_BIN_SMALL_LAST 32
#define MK_BIN_LAST 44
#define MK_BINS (MK_BIN_LAST + 1)

/* Start of the block area of page 0 is rounded to these, so that any block
 * size that is a multiple of a power-of-two alignment A <= block size gives
 * A-aligned blocks (see mk_malloc_aligned). */
#define MK_SMALL_START_ALIGN 8192
#define MK_MEDIUM_START_ALIGN 65536
#define MK_LARGE_START_ALIGN 128

/* We track user virtual addresses below 2^48 in the segment map. */
#define MK_VA_BITS 48

static inline size_t mk_align_up(size_t x, size_t a) { return (x + a - 1) & ~(a - 1); }
static inline size_t mk_align_down(size_t x, size_t a) { return x & ~(a - 1); }
static inline bool mk_is_pow2(size_t x) { return x != 0 && (x & (x - 1)) == 0; }

/* Size -> bin. size 0 maps to bin 1. */
static inline unsigned mk_bin(size_t size) {
  if (size <= 128) return size <= 16 ? 1u : (unsigned)((size + 15) >> 4);
  size_t s = size - 1;
  unsigned b = 63u - (unsigned)__builtin_clzll((unsigned long long)s); /* 2^b <= s */
  return 9u + (b - 7u) * 4u + (unsigned)((s >> (b - 2u)) & 3u);
}

static inline size_t mk_bin_size(unsigned bin) {
  if (bin <= 8) return (size_t)bin * 16;
  unsigned k = bin - 9, b = 7 + k / 4, j = k % 4;
  return ((size_t)1 << b) + (size_t)(j + 1) * ((size_t)1 << (b - 2));
}

/* --------------------------------------------------------------- blocks */

typedef struct mk_block_s {
  struct mk_block_s *next;
} mk_block_t;

/* Low two bits of page->xthread_free: does the next remote free have to
 * tell the owning heap (because the page sits in the heap's full list)? */
#define MK_DELAYED_NONE 0u
#define MK_DELAYED_USE 1u
#define MK_TAG_MASK ((uintptr_t)3)

enum { MK_PAGE_IN_FULL = 1 };
enum { MK_KIND_SMALL = 0, MK_KIND_MEDIUM = 1, MK_KIND_LARGE = 2 };

typedef struct mk_page_s {
  /* fields touched by the fast paths come first */
  mk_block_t *free;       /* blocks ready to hand out */
  mk_block_t *local_free; /* blocks freed by the owner since the last collect */
  uint32_t used;          /* blocks handed out and not yet freed+collected */
  uint32_t capacity;      /* blocks carved so far (lazy extension) */
  uint32_t reserved;      /* blocks that fit in the page */
  uint8_t flags;          /* MK_PAGE_IN_FULL */
  uint8_t bin;
  uint8_t in_use; /* belongs to a heap (small/medium) or is a live large object */
  uint8_t index;  /* position in the segment */
  size_t block_size; /* large: usable bytes of the object */
  _Atomic(uintptr_t) xthread_free; /* remote frees (Treiber stack) + tag */
  uint8_t *start;                  /* first block */
  struct mk_page_s *next, *prev;   /* heap queue */
  uint64_t free_since;             /* ms timestamp when returned to the segment */
#if MK_DEBUG
  _Atomic(uint64_t) *alloc_bits; /* one bit per block: allocated? */
#endif
} mk_page_t;

typedef struct mk_heap_s mk_heap_t;

typedef struct mk_segment_s {
  _Atomic(uintptr_t) thread_id; /* owning thread; 0 when abandoned/large/cached */
  mk_heap_t *heap;              /* owning heap (stable while the segment is in use) */
  size_t size;                  /* bytes mapped, starting at the segment */
  uint8_t kind;
  uint8_t page_shift;
  uint8_t page_count;
  uint8_t is_zero;      /* large: memory known to be zero (fresh from the OS) */
  uint8_t purged_all;   /* cached: everything after the header was purged */
  uint8_t needs_reuse;  /* macOS: some range was purged, call MADV_FREE_REUSE first */
  uint8_t in_free_list; /* linked in heap->free_segs[kind] */
  uint32_t used_pages;
  uint64_t free_mask;   /* pages not handed to a heap queue */
  uint64_t purged_mask; /* free pages whose memory was given back */
  uint64_t cache_since; /* ms timestamp when it entered the cache */
  size_t dirty_extent;  /* bytes from the segment start that may be dirty */
  size_t page0_offset;  /* start of the block area of page 0 */
  struct mk_segment_s *next, *prev;   /* heap->segments or the cache */
  struct mk_segment_s *fnext, *fprev; /* heap->free_segs[kind] */
  mk_page_t pages[];
} mk_segment_t;

typedef struct mk_page_queue_s {
  mk_page_t *first, *last;
} mk_page_queue_t;

struct mk_heap_s {
  mk_page_t *direct[MK_DIRECT_SLOTS]; /* (size+15)/16 -> first page of its bin */
  mk_page_queue_t queues[MK_BINS];
  mk_page_queue_t full;
  _Atomic(mk_block_t *) delayed_free; /* remote frees that hit a full page */
  uintptr_t thread_id;
  mk_segment_t *segments;     /* every small/medium segment we own */
  mk_segment_t *free_segs[2]; /* segments with a free page, per kind */
  size_t segment_count;
  uint64_t next_purge;        /* ms: earliest time for the next purge pass */
  uint64_t free_pages;        /* pages free inside our segments and not purged */
  mk_heap_t *next_abandoned;
  mk_heap_t *next_all;
  bool abandoned;
};

/* ------------------------------------------------------------ the OS */

typedef struct mk_options_s {
  long purge_delay_ms; /* <0: never purge; 0: purge immediately */
  bool print_stats;
  bool tsd_direct; /* macOS: read our TSD slot without calling pthread */
} mk_options_t;

extern mk_internal mk_options_t mk_options;
extern mk_internal size_t mk_os_page_size;

mk_internal void mk_os_init(void);
mk_internal void *mk_os_alloc_aligned(size_t size, size_t align);
mk_internal void *mk_os_alloc_aligned_at(size_t size, size_t align, size_t offset);
mk_internal void mk_os_free(void *p, size_t size);
mk_internal void mk_os_purge(void *p, size_t size);
mk_internal void mk_os_reuse(void *p, size_t size);
mk_internal bool mk_os_try_extend(void *end, size_t extra);
mk_internal uint64_t mk_clock_ms(void);
mk_internal void mk_write_err(const char *s);
mk_internal char *mk_fmt_u64(char *buf, uint64_t v); /* returns end */

/* -------------------------------------------------------------- stats */

typedef struct mk_gstats_s {
  _Atomic(size_t) mapped, peak_mapped, segments, large, cached, purged;
  _Atomic(uint64_t) mmap_calls, munmap_calls, madvise_calls;
  _Atomic(uint64_t) pages_allocated, pages_retired;
  _Atomic(uint64_t) heaps_created, heaps_abandoned, heaps_adopted, abandoned_now;
  _Atomic(uint64_t) remote_frees, delayed_frees, realloc_in_place, realloc_moved;
} mk_gstats_t;
extern mk_internal mk_gstats_t mk_gstats;
#define mk_stat_add(f, v) atomic_fetch_add_explicit(&mk_gstats.f, (v), memory_order_relaxed)
#define mk_stat_sub(f, v) atomic_fetch_sub_explicit(&mk_gstats.f, (v), memory_order_relaxed)

/* ------------------------------------------------------ thread identity */

/* A per-thread unique word that is cheap to read. On Apple arm64 it is the
 * base of the thread-specific-data array (low 3 bits hold the CPU number on
 * some OS versions, so they are masked). */
static inline uintptr_t mk_thread_id(void) {
#if defined(__APPLE__) && defined(__aarch64__)
  uintptr_t t;
  __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(t));
  return t & ~(uintptr_t)7;
#elif defined(__linux__) && defined(__aarch64__)
  uintptr_t t;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(t));
  return t;
#elif defined(__linux__) && defined(__x86_64__)
  uintptr_t t;
  __asm__ volatile("movq %%fs:0, %0" : "=r"(t));
  return t;
#elif defined(__APPLE__) && defined(__x86_64__)
  uintptr_t t;
  __asm__ volatile("movq %%gs:0, %0" : "=r"(t));
  return t;
#else
  return (uintptr_t)pthread_self();
#endif
}

/* --------------------------------------------------------- heap lookup */

extern mk_internal mk_page_t mk_page_empty;

#if defined(__APPLE__)
#define MK_TLS_PTHREAD 1
extern mk_internal pthread_key_t mk_heap_key;
extern mk_internal bool mk_key_ready;
static inline mk_heap_t *mk_heap_get(void) {
#if defined(__aarch64__)
  if (mk_likely(mk_options.tsd_direct)) return ((mk_heap_t **)mk_thread_id())[mk_heap_key];
#endif
  if (mk_unlikely(!__atomic_load_n(&mk_key_ready, __ATOMIC_ACQUIRE))) return NULL;
  return (mk_heap_t *)pthread_getspecific(mk_heap_key);
}
#else
#define MK_TLS_PTHREAD 0
extern __thread mk_heap_t *mk_tls_heap __attribute__((tls_model("initial-exec")));
static inline mk_heap_t *mk_heap_get(void) { return mk_tls_heap; }
#endif

/* ----------------------------------------------- segment map and lookup */

/* One bit per 4 MiB of address space: does a mallockit segment start
 * there? 8 MiB of zero-initialised (untouched, so not resident) memory. */
#define MK_MAP_CHUNKS ((size_t)1 << (MK_VA_BITS - MK_SEGMENT_SHIFT))
extern mk_internal _Atomic(uint64_t) mk_segmap[MK_MAP_CHUNKS / 64];
static inline bool mk_segmap_test(uintptr_t chunk) {
  if (chunk >= MK_MAP_CHUNKS) return false;
  uint64_t w = atomic_load_explicit(&mk_segmap[chunk / 64], memory_order_relaxed);
  return (w >> (chunk % 64)) & 1;
}

/* The segment containing p. Blocks never start at a segment boundary
 * (the header lives there) except for objects with an alignment of at
 * least one segment, whose header sits exactly one segment before them. */
static inline mk_segment_t *mk_segment_of(const void *p) {
  uintptr_t a = (uintptr_t)p;
  if (mk_unlikely((a & MK_SEGMENT_MASK) == 0)) return (mk_segment_t *)(a - MK_SEGMENT_SIZE);
  return (mk_segment_t *)(a & ~(uintptr_t)MK_SEGMENT_MASK);
}

static inline mk_page_t *mk_page_of(mk_segment_t *s, const void *p) {
  return &s->pages[((uintptr_t)p - (uintptr_t)s) >> s->page_shift];
}

static inline mk_segment_t *mk_page_segment(const mk_page_t *pg) {
  return (mk_segment_t *)((uintptr_t)pg & ~(uintptr_t)MK_SEGMENT_MASK);
}

/* ------------------------------------------------------- cross-layer API */

/* segment.c */
mk_internal size_t mk_segment_header_size(int kind);
mk_internal mk_page_t *mk_segment_page_alloc(mk_heap_t *heap, int kind);
mk_internal void mk_segment_page_free(mk_heap_t *heap, mk_page_t *page);
mk_internal void mk_heap_purge(mk_heap_t *heap, bool force);
mk_internal void *mk_large_alloc(size_t size, size_t align);
mk_internal void mk_large_free(mk_segment_t *seg);
mk_internal void *mk_large_realloc_in_place(mk_segment_t *seg, void *p, size_t size);
mk_internal void mk_segment_cache_flush(bool all);
mk_internal void mk_segment_release(mk_segment_t *seg);
mk_internal void mk_segments_fork_prepare(void);
mk_internal void mk_segments_fork_parent(void);
mk_internal void mk_segments_fork_child(void);

/* page.c */
mk_internal void mk_page_init(mk_page_t *page, unsigned bin, size_t area);
mk_internal void mk_page_extend(mk_page_t *page);
mk_internal void mk_page_collect(mk_page_t *page);
mk_internal bool mk_page_to_full(mk_heap_t *heap, mk_page_t *page);
mk_internal void mk_page_unfull(mk_heap_t *heap, mk_page_t *page);
mk_internal void mk_queue_remove(mk_heap_t *heap, mk_page_queue_t *q, mk_page_t *page);
mk_internal void mk_queue_push_front(mk_heap_t *heap, mk_page_queue_t *q, mk_page_t *page);
mk_internal void mk_queue_push_back(mk_heap_t *heap, mk_page_queue_t *q, mk_page_t *page);
mk_internal void mk_heap_update_direct(mk_heap_t *heap, unsigned bin);
mk_internal mk_page_t *mk_find_free_page(mk_heap_t *heap, unsigned bin);
mk_internal void mk_page_retire(mk_heap_t *heap, mk_page_t *page);

/* heap.c */
mk_internal void mk_init(void);
mk_internal mk_heap_t *mk_heap_thread_init(void);
mk_internal void mk_heap_delayed_free_all(mk_heap_t *heap);
mk_internal void mk_heap_collect(mk_heap_t *heap, bool force);
mk_internal void mk_abandoned_reclaim(int max);
mk_internal bool mk_abandoned_pending(void);
mk_internal void mk_fork_prepare(void);
mk_internal void mk_fork_parent(void);
mk_internal void mk_fork_child(void);
extern mk_internal _Atomic(int) mk_init_state;

/* alloc.c */
mk_internal void *mk_malloc_generic(mk_heap_t *heap, size_t size);
mk_internal void mk_free_local_slow(mk_heap_t *heap, mk_page_t *page);
mk_internal void mk_free_block_owned(mk_heap_t *heap, mk_page_t *page, mk_block_t *b);
mk_internal void *mk_malloc_aligned(size_t size, size_t align);
mk_internal size_t mk_block_usable(const void *p);

/* debug.c */
mk_internal void mk_error(int err, const void *p, const char *msg);
#if MK_DEBUG
mk_internal size_t mk_debug_padding(void);
mk_internal void *mk_debug_on_alloc(void *block, size_t block_usable, size_t size);
mk_internal bool mk_debug_on_free(mk_segment_t *seg, mk_page_t *page, void *p);
mk_internal size_t mk_debug_size(const void *p, size_t block_usable);
mk_internal void mk_debug_page_init(mk_segment_t *seg, mk_page_t *page);
mk_internal void mk_debug_check_next(mk_page_t *page, mk_block_t *b);
#endif

/* locks: plain pthread mutexes (static init, no allocation, fork re-init) */
typedef pthread_mutex_t mk_lock_t;
#define MK_LOCK_INIT PTHREAD_MUTEX_INITIALIZER
static inline void mk_lock(mk_lock_t *l) { pthread_mutex_lock(l); }
static inline void mk_unlock(mk_lock_t *l) { pthread_mutex_unlock(l); }
static inline bool mk_trylock(mk_lock_t *l) { return pthread_mutex_trylock(l) == 0; }

#endif
