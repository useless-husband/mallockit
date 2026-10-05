/* Heap layer: one heap per thread.
 *
 * Heaps are never freed: they live in chunks mapped from the OS and are
 * recycled. That makes them "type-stable", so a thread doing a remote free
 * may always touch the heap a segment belongs to, even if that heap's
 * thread is exiting at that moment.
 *
 * When a thread exits, its heap is collected and "abandoned": its
 * segments lose their owner (thread_id 0) and every later free into them
 * takes the atomic remote path. The next thread that starts adopts the
 * whole heap, pages and all. Until then, threads that run out of
 * segments reclaim empty pages from abandoned heaps (mk_abandoned_reclaim). */
#include "internal.h"

#include <sched.h>
#include <stdlib.h>

_Atomic(int) mk_init_state; /* 0: not started, 1: running, 2: done */
static _Atomic(uintptr_t) mk_init_owner;
pthread_key_t mk_heap_key;
bool mk_key_ready;
#if !MK_TLS_PTHREAD
__thread mk_heap_t *mk_tls_heap __attribute__((tls_model("initial-exec")));
#endif

/* The fast path reads page->free without a NULL check on the page. */
mk_page_t mk_page_empty;

static mk_lock_t mk_heaps_lock = MK_LOCK_INIT; /* registry + abandoned list */
static mk_heap_t *mk_heaps_all;
static mk_heap_t *mk_heaps_spare;
static mk_heap_t *mk_abandoned_first, *mk_abandoned_last;
static _Atomic(size_t) mk_abandoned_count;

static void mk_thread_exit(void *value);

/* ------------------------------------------------------------- init */

void mk_init(void) {
  if (mk_likely(atomic_load_explicit(&mk_init_state, memory_order_acquire) == 2)) return;
  uintptr_t me = mk_thread_id();
  int expected = 0;
  if (atomic_compare_exchange_strong(&mk_init_state, &expected, 1)) {
    atomic_store(&mk_init_owner, me);
    mk_os_init();
    if (pthread_key_create(&mk_heap_key, mk_thread_exit) == 0) {
      __atomic_store_n(&mk_key_ready, true, __ATOMIC_RELEASE);
#if defined(__APPLE__) && defined(__aarch64__)
      /* The TSD array is what pthread_getspecific reads; check that our
       * key's slot really is where we think before reading it directly. */
      static int probe;
      if (pthread_setspecific(mk_heap_key, &probe) == 0) {
        void **tsd = (void **)mk_thread_id();
        mk_options.tsd_direct = (tsd[mk_heap_key] == &probe) && getenv("MALLOCKIT_NO_TSD_DIRECT") == NULL;
        pthread_setspecific(mk_heap_key, NULL);
      }
#endif
    }
#if !(MK_OVERRIDE && defined(__APPLE__))
    /* (the macOS zone gets the same calls through its introspection hooks) */
    pthread_atfork(mk_fork_prepare, mk_fork_parent, mk_fork_child);
#endif
    atomic_store_explicit(&mk_init_state, 2, memory_order_release);
    return;
  }
  /* Re-entered from inside init (a libc call that allocates): proceed,
   * everything malloc needs is set up before those calls. */
  if (atomic_load(&mk_init_owner) == me) return;
  while (atomic_load_explicit(&mk_init_state, memory_order_acquire) != 2) sched_yield();
}

/* ------------------------------------------------- heap registry */

#define MK_HEAP_STRIDE ((sizeof(mk_heap_t) + 127) & ~(size_t)127) /* own cache lines */
#define MK_HEAP_CHUNK (64 * 1024)

/* caller holds mk_heaps_lock */
static mk_heap_t *mk_heap_new_locked(void) {
  if (mk_heaps_spare == NULL) {
    uint8_t *chunk = mk_os_alloc_aligned(MK_HEAP_CHUNK, mk_os_page_size);
    if (chunk == NULL) return NULL;
    for (size_t off = 0; off + MK_HEAP_STRIDE <= MK_HEAP_CHUNK; off += MK_HEAP_STRIDE) {
      mk_heap_t *h = (mk_heap_t *)(chunk + off);
      h->next_all = mk_heaps_spare;
      mk_heaps_spare = h;
    }
  }
  mk_heap_t *h = mk_heaps_spare;
  mk_heaps_spare = h->next_all;
  memset(h, 0, sizeof(*h));
  for (size_t i = 0; i < MK_DIRECT_SLOTS; i++) h->direct[i] = &mk_page_empty;
  h->next_all = mk_heaps_all;
  mk_heaps_all = h;
  return h;
}

static void mk_heap_set_owner(mk_heap_t *h, uintptr_t tid) {
  h->thread_id = tid;
  for (mk_segment_t *s = h->segments; s != NULL; s = s->next)
    atomic_store_explicit(&s->thread_id, tid, memory_order_release);
}

static void mk_tls_set(mk_heap_t *h) {
#if !MK_TLS_PTHREAD
  mk_tls_heap = h;
#endif
  if (__atomic_load_n(&mk_key_ready, __ATOMIC_ACQUIRE)) pthread_setspecific(mk_heap_key, h);
}

mk_heap_t *mk_heap_thread_init(void) {
  mk_init();
  mk_heap_t *h = mk_heap_get();
  if (h != NULL) return h;
  uintptr_t tid = mk_thread_id();
  bool adopted = false;
  mk_lock(&mk_heaps_lock);
  h = MK_ADOPT ? mk_abandoned_first : NULL;
  if (h != NULL) {
    mk_abandoned_first = h->next_abandoned;
    if (mk_abandoned_first == NULL) mk_abandoned_last = NULL;
    h->next_abandoned = NULL;
    atomic_fetch_sub_explicit(&mk_abandoned_count, 1, memory_order_relaxed);
    adopted = true;
  } else {
    h = mk_heap_new_locked();
  }
  mk_unlock(&mk_heaps_lock);
  if (h == NULL) return NULL;
  h->abandoned = false;
  mk_heap_set_owner(h, tid);
  if (adopted) {
    mk_stat_add(heaps_adopted, 1);
    mk_stat_sub(abandoned_now, 1);
  } else {
    mk_stat_add(heaps_created, 1);
  }
  mk_tls_set(h);
  return h;
}

/* -------------------------------------------------- collection */

void mk_heap_delayed_free_all(mk_heap_t *heap) {
  if (atomic_load_explicit(&heap->delayed_free, memory_order_relaxed) == NULL) return;
  mk_block_t *b = atomic_exchange_explicit(&heap->delayed_free, NULL, memory_order_acquire);
  while (b != NULL) {
    mk_block_t *next = b->next;
    mk_segment_t *seg = mk_segment_of(b);
    mk_free_block_owned(heap, mk_page_of(seg, b), b);
    mk_stat_add(delayed_frees, 1);
    b = next;
  }
}

/* Merge every pending free, give back every empty page, and purge free
 * pages whose delay has passed (all of them if force). */
void mk_heap_collect(mk_heap_t *heap, bool force) {
  mk_heap_delayed_free_all(heap);
  for (unsigned bin = 1; bin < MK_BINS; bin++) {
    for (mk_page_t *pg = heap->queues[bin].first, *next; pg != NULL; pg = next) {
      next = pg->next;
      mk_page_collect(pg);
      if (pg->used == 0) mk_page_retire(heap, pg);
    }
  }
  for (mk_page_t *pg = heap->full.first, *next; pg != NULL; pg = next) {
    next = pg->next;
    mk_page_collect(pg);
    if (pg->used == 0)
      mk_page_retire(heap, pg);
    else if (pg->free != NULL)
      mk_page_unfull(heap, pg);
  }
  mk_heap_purge(heap, force);
}

static void mk_heap_abandon(mk_heap_t *h) {
  mk_heap_collect(h, false);
  mk_heap_set_owner(h, 0);
  h->abandoned = true;
  mk_lock(&mk_heaps_lock);
  h->next_abandoned = NULL;
  if (mk_abandoned_last) mk_abandoned_last->next_abandoned = h; else mk_abandoned_first = h;
  mk_abandoned_last = h;
  atomic_fetch_add_explicit(&mk_abandoned_count, 1, memory_order_relaxed);
  mk_unlock(&mk_heaps_lock);
  mk_stat_add(heaps_abandoned, 1);
  mk_stat_add(abandoned_now, 1);
}

bool mk_abandoned_pending(void) { return atomic_load_explicit(&mk_abandoned_count, memory_order_relaxed) > 0; }

/* Collect up to max abandoned heaps (all if max < 0). Each heap is taken
 * off the list while it is collected, so its owner-only state has exactly
 * one user; the segments keep thread_id 0, so frees into them (even from
 * this thread) stay on the atomic path. Heaps go back to the end of the
 * list so repeated calls rotate through all of them. */
void mk_abandoned_reclaim(int max) {
  if (!mk_abandoned_pending()) return;
  mk_heap_t *taken = NULL;
  int n = 0;
  if (max < 0) {
    mk_lock(&mk_heaps_lock);
  } else if (!mk_trylock(&mk_heaps_lock)) {
    return;
  }
  while (mk_abandoned_first != NULL && (max < 0 || n < max)) {
    mk_heap_t *h = mk_abandoned_first;
    mk_abandoned_first = h->next_abandoned;
    if (mk_abandoned_first == NULL) mk_abandoned_last = NULL;
    h->next_abandoned = taken;
    taken = h;
    n++;
  }
  atomic_fetch_sub_explicit(&mk_abandoned_count, (size_t)n, memory_order_relaxed);
  mk_unlock(&mk_heaps_lock);
  for (mk_heap_t *h = taken; h != NULL; h = h->next_abandoned) mk_heap_collect(h, max < 0);
  mk_lock(&mk_heaps_lock);
  while (taken != NULL) {
    mk_heap_t *h = taken;
    taken = h->next_abandoned;
    h->next_abandoned = NULL;
    if (mk_abandoned_last) mk_abandoned_last->next_abandoned = h; else mk_abandoned_first = h;
    mk_abandoned_last = h;
  }
  atomic_fetch_add_explicit(&mk_abandoned_count, (size_t)n, memory_order_relaxed);
  mk_unlock(&mk_heaps_lock);
}

/* ---------------------------------------------------- thread exit */

static void mk_thread_exit(void *value) {
  mk_heap_t *h = (mk_heap_t *)value;
  if (h == NULL) return;
#if !MK_TLS_PTHREAD
  mk_tls_heap = NULL;
#endif
  /* pthread already cleared the key. If a later destructor allocates, a new
   * heap is attached and this destructor runs again for it. */
  mk_heap_abandon(h);
}

void mk_thread_done(void) {
  mk_heap_t *h = mk_heap_get();
  if (h == NULL) return;
  mk_tls_set(NULL);
  mk_heap_abandon(h);
}

/* ------------------------------------------------------------ fork */

static bool mk_fork_locked;

void mk_fork_prepare(void) {
  if (atomic_load_explicit(&mk_init_state, memory_order_acquire) != 2) return;
  mk_lock(&mk_heaps_lock);
  mk_segments_fork_prepare();
  mk_fork_locked = true;
}

void mk_fork_parent(void) {
  if (!mk_fork_locked) return;
  mk_fork_locked = false;
  mk_segments_fork_parent();
  mk_unlock(&mk_heaps_lock);
}

/* Only the forking thread exists in the child. Heaps owned by the other
 * threads have no owner any more: clear their segments' thread ids so a
 * new thread that happens to get a dead thread's id cannot treat them as
 * its own. Their memory stays allocated in the child (it may be in use by
 * data the child inherited). The walk is validated because another thread
 * may have been in the middle of changing its segment list. */
void mk_fork_child(void) {
  if (!mk_fork_locked) return;
  mk_fork_locked = false;
  mk_segments_fork_child();
  pthread_mutex_init(&mk_heaps_lock, NULL);
  mk_heap_t *mine = mk_heap_get();
  for (mk_heap_t *h = mk_heaps_all; h != NULL; h = h->next_all) {
    if (h == mine || h->abandoned || h->thread_id == 0) continue;
    size_t n = 0;
    for (mk_segment_t *s = h->segments; s != NULL && n <= h->segment_count; s = s->next, n++) {
      if (!mk_segmap_test((uintptr_t)s >> MK_SEGMENT_SHIFT) || s->heap != h) break;
      atomic_store_explicit(&s->thread_id, 0, memory_order_relaxed);
    }
    h->thread_id = 0;
  }
}

/* ------------------------------------------------------- public */

void mk_collect(bool force) {
  mk_heap_t *h = mk_heap_get();
  if (h != NULL) mk_heap_collect(h, force);
  mk_abandoned_reclaim(-1);
  mk_segment_cache_flush(force);
}

void mk_stats_get(mk_stats_t *o) {
#define G(f) atomic_load_explicit(&mk_gstats.f, memory_order_relaxed)
  memset(o, 0, sizeof(*o));
  o->mapped_bytes = G(mapped);
  o->peak_mapped_bytes = G(peak_mapped);
  o->segments_in_use = G(segments);
  o->large_in_use = G(large);
  o->cached_segments = G(cached);
  o->purged_bytes = G(purged);
  o->mmap_calls = G(mmap_calls);
  o->munmap_calls = G(munmap_calls);
  o->madvise_calls = G(madvise_calls);
  o->pages_allocated = G(pages_allocated);
  o->pages_retired = G(pages_retired);
  o->heaps_created = G(heaps_created);
  o->heaps_abandoned = G(heaps_abandoned);
  o->heaps_adopted = G(heaps_adopted);
  o->abandoned_heaps = G(abandoned_now);
  o->remote_frees = G(remote_frees);
  o->delayed_frees = G(delayed_frees);
  o->realloc_in_place = G(realloc_in_place);
  o->realloc_moved = G(realloc_moved);
#undef G
}

static void mk_line(const char *name, uint64_t v) {
  char buf[96];
  size_t n = strlen(name);
  memcpy(buf, name, n);
  char *e = mk_fmt_u64(buf + n, v);
  *e++ = '\n';
  *e = 0;
  mk_write_err(buf);
}

void mk_stats_print(void) {
  mk_stats_t s;
  mk_stats_get(&s);
  mk_write_err("mallockit stats\n");
  mk_line("  mapped KiB          ", s.mapped_bytes / 1024);
  mk_line("  peak mapped KiB     ", s.peak_mapped_bytes / 1024);
  mk_line("  segments in use     ", s.segments_in_use);
  mk_line("  large objects       ", s.large_in_use);
  mk_line("  cached segments     ", s.cached_segments);
  mk_line("  purged KiB (total)  ", s.purged_bytes / 1024);
  mk_line("  mmap/munmap calls   ", s.mmap_calls);
  mk_line("                      ", s.munmap_calls);
  mk_line("  madvise calls       ", s.madvise_calls);
  mk_line("  pages alloc/retired ", s.pages_allocated);
  mk_line("                      ", s.pages_retired);
  mk_line("  heaps created       ", s.heaps_created);
  mk_line("  heaps abandoned     ", s.heaps_abandoned);
  mk_line("  heaps adopted       ", s.heaps_adopted);
  mk_line("  remote frees        ", s.remote_frees);
  mk_line("  delayed frees       ", s.delayed_frees);
}
