/* Page layer: every page serves one size class (bin) and keeps three free
 * lists ("free-list sharding", as in mimalloc):
 *   free        blocks the allocation fast path pops from;
 *   local_free  blocks freed by the owning thread (no atomics needed);
 *   xthread_free blocks freed by other threads (lock-free stack).
 * When `free` runs dry the slow path merges the other two into it. */
#include "internal.h"

#define MK_EXTEND_BYTES (16 * 1024)
#define MK_EXTEND_MIN_BLOCKS 4

void mk_page_init(mk_page_t *page, unsigned bin, size_t area) {
  size_t bs = mk_bin_size(bin);
  page->bin = (uint8_t)bin;
  page->block_size = bs;
  page->reserved = (uint32_t)(area / bs);
  page->capacity = 0;
  page->used = 0;
  page->free = page->local_free = NULL;
  page->flags = 0;
  page->next = page->prev = NULL;
  atomic_store_explicit(&page->xthread_free, 0, memory_order_relaxed);
}

/* Carve the next few blocks into the free list. Lazily, so that a page
 * only becomes resident as far as it is used. */
void mk_page_extend(mk_page_t *page) {
  size_t bs = page->block_size;
  uint32_t n = page->reserved - page->capacity;
  uint32_t step = (uint32_t)(MK_EXTEND_BYTES / bs);
  if (step < MK_EXTEND_MIN_BLOCKS) step = MK_EXTEND_MIN_BLOCKS;
  if (n > step) n = step;
  if (n == 0) return;
  uint8_t *first = page->start + (size_t)page->capacity * bs;
  uint8_t *p = first;
  for (uint32_t i = 0; i < n; i++, p += bs) {
    ((mk_block_t *)p)->next = (i + 1 < n) ? (mk_block_t *)(p + bs) : page->free;
#if MK_DEBUG
    memset(p + sizeof(mk_block_t), 0xDF, bs - sizeof(mk_block_t));
#endif
  }
  page->free = (mk_block_t *)first;
  page->capacity += n;
}

/* Move remote frees and local frees into `free`. Only the owner calls it. */
void mk_page_collect(mk_page_t *page) {
  uintptr_t tf = atomic_load_explicit(&page->xthread_free, memory_order_relaxed);
  if ((tf & ~MK_TAG_MASK) != 0) {
    while (!atomic_compare_exchange_weak_explicit(&page->xthread_free, &tf, tf & MK_TAG_MASK, memory_order_acquire,
                                                  memory_order_relaxed)) {
    }
    mk_block_t *list = (mk_block_t *)(tf & ~MK_TAG_MASK);
    if (list != NULL) {
      uint32_t count = 1;
      mk_block_t *tail = list;
      while (tail->next != NULL) {
#if MK_DEBUG
        mk_debug_check_next(page, tail->next);
#endif
        tail = tail->next;
        count++;
      }
      tail->next = page->free;
      page->free = list;
      page->used -= count;
      mk_stat_add(remote_frees, count);
    }
  }
  if (page->local_free != NULL && page->free == NULL) {
    page->free = page->local_free;
    page->local_free = NULL;
  }
}

/* --------------------------------------------------------- page queues */

void mk_heap_update_direct(mk_heap_t *heap, unsigned bin) {
  size_t bs = mk_bin_size(bin);
  if (bs > MK_DIRECT_MAX) return;
  mk_page_t *pg = heap->queues[bin].first ? heap->queues[bin].first : &mk_page_empty;
  size_t lo = bin == 1 ? 0 : mk_bin_size(bin - 1) / 16 + 1;
  size_t hi = bs / 16;
  for (size_t w = lo; w <= hi; w++) heap->direct[w] = pg;
}

static void mk_queue_changed(mk_heap_t *heap, mk_page_queue_t *q, mk_page_t *page) {
  if (q != &heap->full) mk_heap_update_direct(heap, page->bin);
}

void mk_queue_remove(mk_heap_t *heap, mk_page_queue_t *q, mk_page_t *page) {
  bool was_first = q->first == page;
  if (page->prev) page->prev->next = page->next; else q->first = page->next;
  if (page->next) page->next->prev = page->prev; else q->last = page->prev;
  page->next = page->prev = NULL;
  if (was_first) mk_queue_changed(heap, q, page);
}

void mk_queue_push_front(mk_heap_t *heap, mk_page_queue_t *q, mk_page_t *page) {
  page->prev = NULL;
  page->next = q->first;
  if (q->first) q->first->prev = page; else q->last = page;
  q->first = page;
  mk_queue_changed(heap, q, page);
}

void mk_queue_push_back(mk_heap_t *heap, mk_page_queue_t *q, mk_page_t *page) {
  page->next = NULL;
  page->prev = q->last;
  if (q->last) q->last->next = page; else q->first = page;
  q->last = page;
  if (q->first == page) mk_queue_changed(heap, q, page);
}

/* ------------------------------------------------------ full pages */

/* Park a page that has no free block. From now on the first remote free
 * into it is routed to heap->delayed_free so that the owner notices.
 * Fails (returns false) if remote frees are already waiting: then the page
 * is not really full. The tag and the pending-list check are one CAS, so a
 * remote free cannot slip in between. */
bool mk_page_to_full(mk_heap_t *heap, mk_page_t *page) {
  uintptr_t tf = atomic_load_explicit(&page->xthread_free, memory_order_relaxed);
  do {
    if ((tf & ~MK_TAG_MASK) != 0) return false;
  } while (!atomic_compare_exchange_weak_explicit(&page->xthread_free, &tf, (uintptr_t)MK_DELAYED_USE,
                                                  memory_order_acq_rel, memory_order_relaxed));
  mk_queue_remove(heap, &heap->queues[page->bin], page);
  page->flags |= MK_PAGE_IN_FULL;
  mk_queue_push_back(heap, &heap->full, page);
  return true;
}

void mk_page_unfull(mk_heap_t *heap, mk_page_t *page) {
  uintptr_t tf = atomic_load_explicit(&page->xthread_free, memory_order_relaxed);
  while (!atomic_compare_exchange_weak_explicit(&page->xthread_free, &tf, tf & ~MK_TAG_MASK, memory_order_acq_rel,
                                                memory_order_relaxed)) {
  }
  mk_queue_remove(heap, &heap->full, page);
  page->flags &= (uint8_t)~MK_PAGE_IN_FULL;
  mk_queue_push_back(heap, &heap->queues[page->bin], page);
}

void mk_page_retire(mk_heap_t *heap, mk_page_t *page) {
  mk_page_queue_t *q = (page->flags & MK_PAGE_IN_FULL) ? &heap->full : &heap->queues[page->bin];
  mk_queue_remove(heap, q, page);
  mk_segment_page_free(heap, page);
}

/* ------------------------------------------------- finding a free page */

mk_page_t *mk_find_free_page(mk_heap_t *heap, unsigned bin) {
  mk_page_queue_t *q = &heap->queues[bin];
  mk_page_t *page = q->first;
  while (page != NULL) {
    mk_page_t *next = page->next;
    mk_page_collect(page);
    if (page->free == NULL && page->capacity < page->reserved) mk_page_extend(page);
    if (page->free != NULL) goto found;
    if (!mk_page_to_full(heap, page)) {
      mk_page_collect(page);
      if (page->free != NULL) goto found;
    }
    page = next;
  }
  page = mk_segment_page_alloc(heap, bin <= MK_BIN_SMALL_LAST ? MK_KIND_SMALL : MK_KIND_MEDIUM);
  if (page == NULL) return NULL;
  mk_page_init(page, bin, page->block_size);
  mk_page_extend(page);
  mk_queue_push_front(heap, q, page);
  return page;
found:
  if (q->first != page) {
    mk_queue_remove(heap, q, page);
    mk_queue_push_front(heap, q, page);
  }
  return page;
}
