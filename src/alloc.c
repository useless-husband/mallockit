/* The public API and its fast paths.
 *
 * malloc fast path (sizes <= 1 KiB): thread heap -> direct[size/16] -> page
 * -> pop page->free. No locks, no atomics, no size-class computation.
 * free fast path: mask the pointer to find the segment; if the segment
 * belongs to this thread, push onto page->local_free. Otherwise one CAS
 * onto the page's remote-free stack. */
#include "internal.h"

#include <errno.h>

/* ------------------------------------------------------------ malloc */

static inline void *mk_page_pop(mk_page_t *page) {
  mk_block_t *b = page->free;
  page->free = b->next;
  page->used++;
#if MK_DEBUG
  if (page->free) mk_debug_check_next(page, page->free);
#endif
  return b;
}

mk_noinline void *mk_malloc_generic(mk_heap_t *heap, size_t size) {
  if (mk_unlikely(heap == NULL)) {
    heap = mk_heap_thread_init();
    if (heap == NULL) {
      errno = ENOMEM;
      return NULL;
    }
  }
  mk_heap_delayed_free_all(heap);
  if (size > MK_MEDIUM_MAX) {
    void *p = mk_large_alloc(size, 0);
    if (p == NULL) errno = ENOMEM;
    return p;
  }
  unsigned bin = mk_bin(size);
  mk_page_t *page = heap->queues[bin].first;
  if (page == NULL || page->free == NULL) page = mk_find_free_page(heap, bin);
  if (page == NULL) {
    errno = ENOMEM;
    return NULL;
  }
  return mk_page_pop(page);
}

static inline void *mk_alloc_raw(size_t size) {
  mk_heap_t *heap = mk_heap_get();
  if (mk_likely(size <= MK_DIRECT_MAX && heap != NULL)) {
    mk_page_t *page = heap->direct[(size + 15) >> 4];
    mk_block_t *b = page->free;
    if (mk_likely(b != NULL)) {
      page->free = b->next;
      page->used++;
#if MK_DEBUG
      if (page->free) mk_debug_check_next(page, page->free);
#endif
      return b;
    }
  }
  return mk_malloc_generic(heap, size);
}

/* Alignment > 16. Every block of a bin whose size is a multiple of a
 * power-of-two A is A-aligned, because page block areas start at a
 * multiple of the largest block size of their kind (8 KiB small, 64 KiB
 * medium) and bins are closed under that rounding (tests/t_sizeclass.c
 * checks it). So rounding the size up is enough: no header, no interior
 * pointers, free() needs no special case. */
static void *mk_alloc_aligned_raw(size_t size, size_t align) {
  if (align <= MK_MIN_ALIGN) return mk_alloc_raw(size);
  size_t sz = mk_align_up(size == 0 ? 1 : size, align);
  if (sz < size) {
    errno = ENOMEM;
    return NULL;
  }
  if (sz <= MK_MEDIUM_MAX) return mk_alloc_raw(sz);
  mk_init();
  void *p = mk_large_alloc(size, align);
  if (p == NULL) errno = ENOMEM;
  return p;
}

static size_t mk_raw_usable(const void *p) {
  mk_segment_t *seg = mk_segment_of(p);
  return mk_page_of(seg, p)->block_size;
}

#if MK_DEBUG
static void *mk_debug_alloc(size_t size, size_t align) {
  size_t need = size + mk_debug_padding();
  if (need < size) {
    errno = ENOMEM;
    return NULL;
  }
  void *p = mk_alloc_aligned_raw(need, align);
  if (p == NULL) return NULL;
  return mk_debug_on_alloc(p, mk_raw_usable(p), size);
}
#endif

void *mk_malloc(size_t size) {
#if MK_DEBUG
  return mk_debug_alloc(size, MK_MIN_ALIGN);
#else
  return mk_alloc_raw(size);
#endif
}

void *mk_malloc_aligned(size_t size, size_t align) {
#if MK_DEBUG
  return mk_debug_alloc(size, align);
#else
  return mk_alloc_aligned_raw(size, align);
#endif
}

/* -------------------------------------------------------------- free */

void mk_free_local_slow(mk_heap_t *heap, mk_page_t *page) {
  if (page->flags & MK_PAGE_IN_FULL) mk_page_unfull(heap, page);
  if (page->used == 0) {
    /* Keep the last page of a bin: a program that allocates and frees one
     * object in a loop would otherwise map and retire a page each time. */
    mk_page_queue_t *q = &heap->queues[page->bin];
    if (MK_KEEP_LAST_PAGE && q->first == page && page->next == NULL) return;
    mk_page_retire(heap, page);
  }
}

/* Owner-side free. With MK_LOCAL_FREE the block goes to local_free and only
 * becomes allocatable again when `free` runs dry, which guarantees the slow
 * path (remote frees, purging) runs regularly and keeps freshly freed
 * blocks out of the way of a hot allocation sequence. */
static inline void mk_push_owned(mk_page_t *page, mk_block_t *b) {
#if MK_LOCAL_FREE
  b->next = page->local_free;
  page->local_free = b;
#else
  b->next = page->free;
  page->free = b;
#endif
}

void mk_free_block_owned(mk_heap_t *heap, mk_page_t *page, mk_block_t *b) {
  mk_push_owned(page, b);
  if (mk_unlikely(--page->used == 0 || page->flags != 0)) mk_free_local_slow(heap, page);
}

/* A free from a thread that does not own the segment. */
static mk_noinline void mk_free_remote(mk_segment_t *seg, mk_page_t *page, void *p) {
  if (seg->kind == MK_KIND_LARGE) {
    mk_large_free(seg);
    return;
  }
  /* Read the heap before publishing the block: once the block is visible
   * the owner may free the whole segment. Heaps are never freed, so the
   * pointer stays usable even if the owner thread is exiting right now. */
  mk_heap_t *heap = seg->heap;
  mk_block_t *b = (mk_block_t *)p;
  uintptr_t tf = atomic_load_explicit(&page->xthread_free, memory_order_relaxed);
  uintptr_t nt;
  bool delayed;
  do {
    if ((tf & MK_TAG_MASK) == MK_DELAYED_USE) {
      /* The page is parked in the owner's full list: clear the tag and
       * hand this block to the owner heap instead, so the owner sees it. */
      nt = (tf & ~MK_TAG_MASK) | MK_DELAYED_NONE;
      delayed = true;
    } else {
      b->next = (mk_block_t *)(tf & ~MK_TAG_MASK);
      nt = (uintptr_t)b | (tf & MK_TAG_MASK);
      delayed = false;
    }
  } while (!atomic_compare_exchange_weak_explicit(&page->xthread_free, &tf, nt, memory_order_release, memory_order_relaxed));
  if (delayed) {
    mk_block_t *head = atomic_load_explicit(&heap->delayed_free, memory_order_relaxed);
    do {
      b->next = head;
    } while (!atomic_compare_exchange_weak_explicit(&heap->delayed_free, &head, b, memory_order_release,
                                                    memory_order_relaxed));
  }
}

void mk_free(void *p) {
  uintptr_t a = (uintptr_t)p;
  mk_segment_t *seg;
  if (mk_unlikely((a & MK_SEGMENT_MASK) == 0)) {
    if (p == NULL) return;
    seg = (mk_segment_t *)(a - MK_SEGMENT_SIZE); /* segment-aligned large object */
  } else {
    seg = (mk_segment_t *)(a & ~(uintptr_t)MK_SEGMENT_MASK);
  }
  mk_page_t *page = mk_page_of(seg, p);
#if MK_DEBUG
  if (!mk_debug_on_free(seg, page, p)) return;
#endif
  if (mk_likely(atomic_load_explicit(&seg->thread_id, memory_order_relaxed) == mk_thread_id())) {
    mk_push_owned(page, (mk_block_t *)p);
    if (mk_unlikely(--page->used == 0 || page->flags != 0)) mk_free_local_slow(seg->heap, page);
  } else {
    mk_free_remote(seg, page, p);
  }
}

/* ------------------------------------------------- the rest of the API */

void *mk_calloc(size_t count, size_t size) {
  size_t total;
  if (__builtin_mul_overflow(count, size, &total)) {
    errno = ENOMEM;
    return NULL;
  }
  void *p = mk_malloc(total);
  if (p == NULL) return NULL;
  if (total > MK_MEDIUM_MAX) {
    mk_segment_t *seg = mk_segment_of(p);
    if (seg->kind == MK_KIND_LARGE && seg->is_zero) return p; /* fresh from mmap */
  }
  memset(p, 0, total);
  return p;
}

void *mk_realloc(void *p, size_t size) {
  if (p == NULL) return mk_malloc(size);
#if MK_DEBUG
  size_t old = mk_usable_size(p);
  void *q = mk_malloc(size);
  if (q == NULL) return NULL;
  memcpy(q, p, old < size ? old : size);
  mk_free(p);
  return q;
#else
  mk_segment_t *seg = mk_segment_of(p);
  mk_page_t *page = mk_page_of(seg, p);
  size_t usable = page->block_size;
  if (seg->kind == MK_KIND_LARGE) {
    if (size > MK_MEDIUM_MAX) {
      if (mk_large_realloc_in_place(seg, p, size) != NULL) {
        mk_stat_add(realloc_in_place, 1);
        return p;
      }
      mk_stat_add(realloc_moved, 1);
    }
  } else if (size <= usable && size >= usable / 2) {
    return p; /* same block still fits and is not more than half empty */
  }
  void *q = mk_malloc(size);
  if (q == NULL) return NULL;
  memcpy(q, p, usable < size ? usable : size);
  mk_free(p);
  return q;
#endif
}

int mk_posix_memalign(void **out, size_t alignment, size_t size) {
  if (alignment < sizeof(void *) || !mk_is_pow2(alignment)) return EINVAL;
  void *p = mk_malloc_aligned(size, alignment);
  if (p == NULL) return ENOMEM;
  *out = p;
  return 0;
}

void *mk_aligned_alloc(size_t alignment, size_t size) {
  if (!mk_is_pow2(alignment)) {
    errno = EINVAL;
    return NULL;
  }
  return mk_malloc_aligned(size, alignment);
}

void *mk_memalign(size_t alignment, size_t size) {
  /* glibc rounds a non-power-of-two alignment up instead of failing */
  if (alignment > ((size_t)1 << 62)) {
    errno = EINVAL;
    return NULL;
  }
  size_t a = 1;
  while (a < alignment) a <<= 1;
  return mk_malloc_aligned(size, a);
}

void *mk_valloc(size_t size) {
  mk_init();
  return mk_malloc_aligned(size, mk_os_page_size);
}

void *mk_pvalloc(size_t size) {
  mk_init();
  size_t sz = mk_align_up(size == 0 ? 1 : size, mk_os_page_size);
  if (sz < size) {
    errno = ENOMEM;
    return NULL;
  }
  return mk_malloc_aligned(sz, mk_os_page_size);
}

size_t mk_usable_size(const void *p) {
  if (p == NULL) return 0;
#if MK_DEBUG
  return mk_debug_size(p, mk_raw_usable(p));
#else
  return mk_raw_usable(p);
#endif
}

size_t mk_good_size(size_t size) {
#if MK_DEBUG
  return size;
#else
  if (size <= MK_MEDIUM_MAX) return mk_bin_size(mk_bin(size));
  mk_init();
  size_t r = mk_align_up(size, mk_os_page_size);
  return r < size ? size : r;
#endif
}

bool mk_owns(const void *p) {
  uintptr_t a = (uintptr_t)p;
  if (a == 0 || (a >> MK_VA_BITS) != 0) return false;
  uintptr_t chunk = a >> MK_SEGMENT_SHIFT;
  if ((a & MK_SEGMENT_MASK) == 0) {
    /* only a segment-aligned large object may start on a boundary */
    if (chunk == 0 || !mk_segmap_test(chunk - 1)) return false;
    mk_segment_t *s = (mk_segment_t *)(a - MK_SEGMENT_SIZE);
    return s->kind == MK_KIND_LARGE && s->pages[0].start == (const uint8_t *)p;
  }
  return mk_segmap_test(chunk);
}

/* For the macOS zone's size(): bytes at p if p is the start of a block of
 * a page in use, else 0. */
size_t mk_block_usable(const void *p) {
  if (!mk_owns(p)) return 0;
  mk_segment_t *seg = mk_segment_of(p);
  mk_page_t *page = mk_page_of(seg, p);
  if (page->in_use == 0 || (const uint8_t *)p < page->start) return 0;
  if (seg->kind == MK_KIND_LARGE) {
    if ((const uint8_t *)p != page->start) return 0;
  } else {
    size_t off = (size_t)((const uint8_t *)p - page->start);
    if (off % page->block_size != 0 || off / page->block_size >= page->capacity) return 0;
  }
  return mk_usable_size(p);
}

bool mk_is_debug_build(void) { return MK_DEBUG != 0; }
