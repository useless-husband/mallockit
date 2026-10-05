/* Segment layer.
 *
 * A segment is 4 MiB of address space aligned to 4 MiB, so the segment of
 * any block is found by masking the pointer. Its header holds the page
 * descriptors. Small segments hold 64 pages of 64 KiB, medium segments 8
 * pages of 512 KiB, and a large segment holds exactly one object.
 *
 * Memory goes back to the OS in two steps (the purge policy):
 *   - a page that becomes free inside a live segment is purged (madvise)
 *     once it has stayed free for MALLOCKIT_PURGE_DELAY ms (default 10);
 *   - an empty segment goes to a small global cache, is purged after the
 *     same delay, and is unmapped when the cache overflows or on
 *     mk_collect(true). */
#include "internal.h"

#include <errno.h>

#define MK_CACHE_MAX 16

/* ------------------------------------------------------- segment map */

#define MK_MAP_CHUNKS ((size_t)1 << (MK_VA_BITS - MK_SEGMENT_SHIFT))
static _Atomic(uint64_t) mk_segmap[MK_MAP_CHUNKS / 64];

bool mk_segmap_test(uintptr_t chunk) {
  if (chunk >= MK_MAP_CHUNKS) return false;
  uint64_t w = atomic_load_explicit(&mk_segmap[chunk / 64], memory_order_relaxed);
  return (w >> (chunk % 64)) & 1;
}

static void mk_segmap_set(mk_segment_t *seg, bool on) {
  uintptr_t chunk = (uintptr_t)seg >> MK_SEGMENT_SHIFT;
  if (chunk >= MK_MAP_CHUNKS) return;
  uint64_t bit = (uint64_t)1 << (chunk % 64);
  if (on)
    atomic_fetch_or_explicit(&mk_segmap[chunk / 64], bit, memory_order_release);
  else
    atomic_fetch_and_explicit(&mk_segmap[chunk / 64], ~bit, memory_order_release);
}

/* -------------------------------------------------------- header sizes */

static unsigned mk_kind_pages(int kind) {
  return kind == MK_KIND_SMALL ? MK_SMALL_PAGES : kind == MK_KIND_MEDIUM ? MK_MEDIUM_PAGES : 1;
}
static unsigned mk_kind_shift(int kind) {
  return kind == MK_KIND_SMALL ? MK_SMALL_PAGE_SHIFT : kind == MK_KIND_MEDIUM ? MK_MEDIUM_PAGE_SHIFT : MK_LARGE_PAGE_SHIFT;
}

#if MK_DEBUG
/* allocation bitmap words per page: page size / smallest block of the kind */
static size_t mk_debug_words(int kind) {
  if (kind == MK_KIND_SMALL) return ((size_t)1 << MK_SMALL_PAGE_SHIFT) / 16 / 64;
  if (kind == MK_KIND_MEDIUM) return (((size_t)1 << MK_MEDIUM_PAGE_SHIFT) / mk_bin_size(MK_BIN_SMALL_LAST + 1) + 63) / 64;
  return 1;
}
#endif

size_t mk_segment_header_size(int kind) {
  size_t h = sizeof(mk_segment_t) + mk_kind_pages(kind) * sizeof(mk_page_t);
#if MK_DEBUG
  h = mk_align_up(h, 8) + mk_kind_pages(kind) * mk_debug_words(kind) * 8;
#endif
  size_t a = kind == MK_KIND_SMALL ? MK_SMALL_START_ALIGN : kind == MK_KIND_MEDIUM ? MK_MEDIUM_START_ALIGN : MK_LARGE_START_ALIGN;
  return mk_align_up(h, a);
}

static void mk_segment_init(mk_segment_t *seg, int kind, size_t page0_offset) {
  /* keep what describes the mapping itself */
  size_t size = seg->size, dirty = seg->dirty_extent;
  uint8_t is_zero = seg->is_zero;
  unsigned n = mk_kind_pages(kind);
  memset(seg, 0, sizeof(mk_segment_t) + n * sizeof(mk_page_t));
  seg->size = size;
  seg->dirty_extent = dirty;
  seg->is_zero = is_zero;
  seg->kind = (uint8_t)kind;
  seg->page_shift = (uint8_t)mk_kind_shift(kind);
  seg->page_count = (uint8_t)n;
  seg->page0_offset = page0_offset;
  seg->free_mask = n == 64 ? ~(uint64_t)0 : (((uint64_t)1 << n) - 1);
  for (unsigned i = 0; i < n; i++) seg->pages[i].index = (uint8_t)i;
#if MK_DEBUG
  _Atomic(uint64_t) *bits = (_Atomic(uint64_t) *)((uint8_t *)seg + mk_align_up(sizeof(mk_segment_t) + n * sizeof(mk_page_t), 8));
  size_t words = mk_debug_words(kind);
  memset((void *)bits, 0, n * words * 8);
  for (unsigned i = 0; i < n; i++) seg->pages[i].alloc_bits = bits + i * words;
#endif
}

/* Block area of page idx. */
static uint8_t *mk_page_area(mk_segment_t *seg, unsigned idx, size_t *size) {
  size_t psize = (size_t)1 << seg->page_shift;
  size_t off = (size_t)idx << seg->page_shift;
  if (idx == 0) {
    *size = psize - seg->page0_offset;
    return (uint8_t *)seg + seg->page0_offset;
  }
  *size = psize;
  return (uint8_t *)seg + off;
}

/* ----------------------------------------------------- segment cache */

static mk_lock_t mk_cache_lock = MK_LOCK_INIT;
static mk_segment_t *mk_cache_first, *mk_cache_last;
static _Atomic(size_t) mk_cache_count; /* written under the lock, peeked without */

static void mk_cache_unlink(mk_segment_t *s) {
  if (s->prev) s->prev->next = s->next; else mk_cache_first = s->next;
  if (s->next) s->next->prev = s->prev; else mk_cache_last = s->prev;
  s->next = s->prev = NULL;
  atomic_fetch_sub_explicit(&mk_cache_count, 1, memory_order_relaxed);
  mk_stat_sub(cached, 1);
}

static void mk_segment_purge_cached(mk_segment_t *s) {
  if (s->purged_all) return;
  /* the first OS page keeps the header fields the cache needs */
  size_t from = mk_os_page_size;
  size_t to = s->dirty_extent > s->size ? s->size : s->dirty_extent;
  if (to > from) mk_os_purge((uint8_t *)s + from, to - from);
  s->purged_all = 1;
  s->needs_reuse = 1;
  s->dirty_extent = from;
}

/* caller holds the cache lock */
static void mk_cache_purge_expired(uint64_t now, bool force) {
  long d = mk_options.purge_delay_ms;
  if (d < 0 && !force) return;
  for (mk_segment_t *s = mk_cache_first; s != NULL; s = s->next) {
    if (!s->purged_all && (force || now >= s->cache_since + (uint64_t)d)) mk_segment_purge_cached(s);
  }
}

static void mk_segment_unmap(mk_segment_t *s) {
  mk_segmap_set(s, false);
  mk_os_free(s, s->size);
}

void mk_segment_release(mk_segment_t *seg) {
  atomic_store_explicit(&seg->thread_id, 0, memory_order_relaxed);
  seg->heap = NULL;
  seg->in_free_list = 0;
  seg->next = seg->prev = seg->fnext = seg->fprev = NULL;
  if (seg->size != MK_SEGMENT_SIZE) {
    mk_segment_unmap(seg);
    return;
  }
  uint64_t now = mk_clock_ms();
  mk_segment_t *evict = NULL;
  mk_lock(&mk_cache_lock);
  mk_cache_purge_expired(now, false);
  if (atomic_load_explicit(&mk_cache_count, memory_order_relaxed) >= MK_CACHE_MAX) {
    evict = mk_cache_last;
    mk_cache_unlink(evict);
  }
  seg->cache_since = now;
  seg->purged_all = 0;
  seg->next = mk_cache_first;
  if (mk_cache_first) mk_cache_first->prev = seg; else mk_cache_last = seg;
  mk_cache_first = seg;
  atomic_fetch_add_explicit(&mk_cache_count, 1, memory_order_relaxed);
  mk_stat_add(cached, 1);
  if (mk_options.purge_delay_ms == 0) mk_segment_purge_cached(seg);
  mk_unlock(&mk_cache_lock);
  if (evict) mk_segment_unmap(evict);
}

void mk_segment_cache_flush(bool all) {
  mk_segment_t *list = NULL;
  mk_lock(&mk_cache_lock);
  if (all) {
    list = mk_cache_first;
    for (mk_segment_t *s = list; s; s = s->next) mk_stat_sub(cached, 1);
    mk_cache_first = mk_cache_last = NULL;
    atomic_store_explicit(&mk_cache_count, 0, memory_order_relaxed);
  } else {
    mk_cache_purge_expired(mk_clock_ms(), true);
  }
  mk_unlock(&mk_cache_lock);
  while (list) {
    mk_segment_t *next = list->next;
    mk_segment_unmap(list);
    list = next;
  }
}

/* A 4 MiB segment from the cache (most recently freed first: still in the
 * CPU caches, no page faults) or from the OS. */
static mk_segment_t *mk_segment_alloc_std(void) {
  mk_segment_t *seg = NULL;
  if (atomic_load_explicit(&mk_cache_count, memory_order_relaxed) > 0) { /* peek; re-checked under the lock */
    mk_lock(&mk_cache_lock);
    seg = mk_cache_first;
    if (seg) mk_cache_unlink(seg);
    mk_unlock(&mk_cache_lock);
  }
  if (seg) {
    if (seg->needs_reuse) {
      mk_os_reuse((uint8_t *)seg + mk_os_page_size, seg->size - mk_os_page_size);
      seg->needs_reuse = 0;
    }
    seg->is_zero = 0;
    return seg;
  }
  seg = mk_os_alloc_aligned(MK_SEGMENT_SIZE, MK_SEGMENT_SIZE);
  if (seg == NULL) return NULL;
  seg->size = MK_SEGMENT_SIZE;
  seg->dirty_extent = 0;
  seg->is_zero = 1;
  seg->needs_reuse = 0;
  mk_segmap_set(seg, true);
  return seg;
}

/* ----------------------------------------------- pages for the heaps */

static void mk_free_segs_push(mk_heap_t *heap, mk_segment_t *seg) {
  mk_segment_t **head = &heap->free_segs[seg->kind];
  seg->fprev = NULL;
  seg->fnext = *head;
  if (*head) (*head)->fprev = seg;
  *head = seg;
  seg->in_free_list = 1;
}

static void mk_free_segs_remove(mk_heap_t *heap, mk_segment_t *seg) {
  if (!seg->in_free_list) return;
  if (seg->fprev) seg->fprev->fnext = seg->fnext; else heap->free_segs[seg->kind] = seg->fnext;
  if (seg->fnext) seg->fnext->fprev = seg->fprev;
  seg->fnext = seg->fprev = NULL;
  seg->in_free_list = 0;
}

mk_page_t *mk_segment_page_alloc(mk_heap_t *heap, int kind) {
  mk_segment_t *seg = heap->free_segs[kind];
  if (seg == NULL) {
    /* Before asking the OS, let memory stranded in the heaps of exited
     * threads flow back into the cache. */
    if (atomic_load_explicit(&mk_cache_count, memory_order_relaxed) == 0 && mk_abandoned_pending()) mk_abandoned_reclaim(1);
    seg = mk_segment_alloc_std();
    if (seg == NULL) return NULL;
    mk_segment_init(seg, kind, mk_segment_header_size(kind));
    if (seg->dirty_extent < seg->page0_offset) seg->dirty_extent = seg->page0_offset;
    seg->heap = heap;
    atomic_store_explicit(&seg->thread_id, heap->thread_id, memory_order_relaxed);
    seg->next = heap->segments;
    if (heap->segments) heap->segments->prev = seg;
    heap->segments = seg;
    heap->segment_count++;
    mk_free_segs_push(heap, seg);
    mk_stat_add(segments, 1);
  }
  uint64_t avail = seg->free_mask;
  uint64_t dirty = avail & ~seg->purged_mask;
  unsigned idx = (unsigned)__builtin_ctzll(dirty ? dirty : avail);
  uint64_t bit = (uint64_t)1 << idx;
  if (seg->purged_mask & bit) {
    size_t sz;
    uint8_t *area = mk_page_area(seg, idx, &sz);
    mk_os_reuse(area, sz);
    seg->purged_mask &= ~bit;
  }
  seg->free_mask &= ~bit;
  seg->used_pages++;
  if (seg->free_mask == 0) mk_free_segs_remove(heap, seg);
  mk_page_t *page = &seg->pages[idx];
  size_t area;
  page->start = mk_page_area(seg, idx, &area);
  page->in_use = 1;
  page->block_size = area; /* mk_page_init sets the real value */
  size_t end = (size_t)(page->start - (uint8_t *)seg) + area;
  if (end > seg->dirty_extent) seg->dirty_extent = end;
  mk_stat_add(pages_allocated, 1);
#if MK_DEBUG
  mk_debug_page_init(seg, page);
#endif
  return page;
}

static void mk_page_purge(mk_segment_t *seg, unsigned idx) {
  size_t sz;
  uint8_t *area = mk_page_area(seg, idx, &sz);
  mk_os_purge(area, sz);
  seg->purged_mask |= (uint64_t)1 << idx;
}

void mk_segment_page_free(mk_heap_t *heap, mk_page_t *page) {
  mk_segment_t *seg = mk_page_segment(page);
  unsigned idx = page->index;
  uint64_t bit = (uint64_t)1 << idx;
  page->in_use = 0;
  page->free = page->local_free = NULL;
  page->used = page->capacity = page->reserved = 0;
  page->flags = 0;
  page->next = page->prev = NULL;
  atomic_store_explicit(&page->xthread_free, 0, memory_order_relaxed);
  seg->free_mask |= bit;
  seg->used_pages--;
  mk_stat_add(pages_retired, 1);
  if (seg->used_pages == 0) {
    mk_free_segs_remove(heap, seg);
    if (seg->prev) seg->prev->next = seg->next; else heap->segments = seg->next;
    if (seg->next) seg->next->prev = seg->prev;
    heap->segment_count--;
    mk_stat_sub(segments, 1);
    if (seg->purged_mask != 0) seg->needs_reuse = 1;
    seg->dirty_extent = MK_SEGMENT_SIZE;
    mk_segment_release(seg);
    return;
  }
  if (!seg->in_free_list) mk_free_segs_push(heap, seg);
  long d = mk_options.purge_delay_ms;
  if (d == 0) {
    mk_page_purge(seg, idx);
  } else if (d > 0) {
    uint64_t now = mk_clock_ms();
    page->free_since = now;
    if (now >= heap->next_purge) mk_heap_purge(heap, false);
  }
}

/* Purge pages that have been free for longer than the delay. */
void mk_heap_purge(mk_heap_t *heap, bool force) {
  long d = mk_options.purge_delay_ms;
  if (d < 0 && !force) return;
  uint64_t now = mk_clock_ms();
  for (int kind = 0; kind < 2; kind++) {
    for (mk_segment_t *seg = heap->free_segs[kind]; seg != NULL; seg = seg->fnext) {
      uint64_t m = seg->free_mask & ~seg->purged_mask;
      while (m) {
        unsigned idx = (unsigned)__builtin_ctzll(m);
        m &= m - 1;
        if (force || now >= seg->pages[idx].free_since + (uint64_t)d) mk_page_purge(seg, idx);
      }
    }
  }
  heap->next_purge = now + (uint64_t)(d > 0 ? d : 1);
}

/* ------------------------------------------------------ large objects */

static void *mk_large_setup(mk_segment_t *seg, size_t off, size_t need) {
  mk_segment_init(seg, MK_KIND_LARGE, off);
  seg->used_pages = 1;
  seg->free_mask = 0;
  mk_page_t *page = &seg->pages[0];
  page->start = (uint8_t *)seg + off;
  page->block_size = need - off;
  page->reserved = page->capacity = page->used = 1;
  page->in_use = 1;
  if (need > seg->dirty_extent) seg->dirty_extent = need;
  mk_stat_add(large, 1);
#if MK_DEBUG
  mk_debug_page_init(seg, page);
#endif
  return page->start;
}

/* Objects aligned to a segment or more: the header goes in the segment
 * right before the object, which keeps mk_segment_of() a subtraction. */
static void *mk_huge_aligned_alloc(size_t size, size_t align) {
  size_t body = mk_align_up(size, mk_os_page_size);
  if (body < size) return NULL;
  mk_segment_t *seg = mk_os_alloc_aligned_at(MK_SEGMENT_SIZE + body, align, MK_SEGMENT_SIZE);
  if (seg == NULL) return NULL;
  seg->size = MK_SEGMENT_SIZE + body;
  seg->dirty_extent = 0;
  seg->is_zero = 1;
  mk_segmap_set(seg, true);
  return mk_large_setup(seg, MK_SEGMENT_SIZE, MK_SEGMENT_SIZE + body);
}

void *mk_large_alloc(size_t size, size_t align) {
  if (size > ((size_t)1 << MK_VA_BITS)) {
    errno = ENOMEM;
    return NULL;
  }
  if (align < MK_MIN_ALIGN) align = MK_MIN_ALIGN;
  if (align >= MK_SEGMENT_SIZE) return mk_huge_aligned_alloc(size, align);
  size_t off = mk_align_up(mk_segment_header_size(MK_KIND_LARGE), align > MK_LARGE_START_ALIGN ? align : MK_LARGE_START_ALIGN);
  size_t need = mk_align_up(off + size, mk_os_page_size);
  mk_segment_t *seg;
  if (need <= MK_SEGMENT_SIZE) {
    seg = mk_segment_alloc_std();
    if (seg == NULL) return NULL;
    /* A reused segment may hold up to 4 MiB of dirty pages; do not let a
     * small large object keep them all resident. */
    if (seg->dirty_extent > need + 4 * mk_os_page_size) {
      mk_os_purge((uint8_t *)seg + need, seg->dirty_extent - need);
      seg->needs_reuse = 1;
      seg->dirty_extent = need;
    }
  } else {
    seg = mk_os_alloc_aligned(need, MK_SEGMENT_SIZE);
    if (seg == NULL) return NULL;
    seg->size = need;
    seg->dirty_extent = 0;
    seg->is_zero = 1;
    seg->needs_reuse = 0;
    mk_segmap_set(seg, true);
  }
  return mk_large_setup(seg, off, need);
}

void mk_large_free(mk_segment_t *seg) {
  mk_stat_sub(large, 1);
  mk_page_t *page = &seg->pages[0];
  page->in_use = 0;
  size_t end = (size_t)(page->start - (uint8_t *)seg) + page->block_size;
  if (end > seg->dirty_extent) seg->dirty_extent = end;
  mk_segment_release(seg);
}

/* Grow or shrink a large object without moving it. Returns NULL if it
 * cannot (the caller then moves it). */
void *mk_large_realloc_in_place(mk_segment_t *seg, void *p, size_t size) {
  mk_page_t *page = &seg->pages[0];
  size_t off = (size_t)(page->start - (uint8_t *)seg);
  if (size > ((size_t)1 << MK_VA_BITS)) return NULL;
  size_t need = mk_align_up(off + size, mk_os_page_size);
  size_t old_end = off + page->block_size;
  if (need <= old_end) {
    /* shrink: hand back the tail if it is worth a system call */
    if (old_end - need >= 16 * mk_os_page_size) {
      mk_os_purge((uint8_t *)seg + need, old_end - need);
      seg->needs_reuse = 1;
      seg->dirty_extent = need;
    }
    page->block_size = need - off;
    return p;
  }
  if (need > seg->size) {
    if (!mk_os_try_extend((uint8_t *)seg + seg->size, need - seg->size)) return NULL;
    seg->size = need;
  }
#if defined(__APPLE__)
  if (seg->needs_reuse) mk_os_reuse((uint8_t *)seg + old_end, need - old_end);
#endif
  page->block_size = need - off;
  if (need > seg->dirty_extent) seg->dirty_extent = need;
  return p;
}

/* ------------------------------------------------------------ fork */

void mk_segments_fork_prepare(void) { mk_lock(&mk_cache_lock); }
void mk_segments_fork_parent(void) { mk_unlock(&mk_cache_lock); }
void mk_segments_fork_child(void) { pthread_mutex_init(&mk_cache_lock, NULL); }
