/* Page layer: lazy extension, the three free lists, full pages. */
#include "test.h"

#include <pthread.h>

TEST(page_extension_is_lazy_and_well_formed) {
  void *p = mk_malloc(32);
  mk_page_t *pg = mk_page_of(mk_segment_of(p), p);
  CHECK(pg->capacity <= pg->reserved);
  CHECK(pg->reserved == (uint32_t)((((size_t)1 << MK_SMALL_PAGE_SHIFT) - (pg->index == 0 ? mk_segment_header_size(MK_KIND_SMALL) : 0)) / 32));
  CHECK((size_t)pg->capacity * 32 <= 16 * 1024 || pg->capacity == 4); /* not the whole page at once */
  /* walk the free list: distinct, in range, on block boundaries */
  uint32_t n = 0;
  for (mk_block_t *b = pg->free; b != NULL; b = b->next) {
    uintptr_t off = (uintptr_t)b - (uintptr_t)pg->start;
    CHECK(off % 32 == 0 && off / 32 < pg->capacity);
    CHECK(++n <= pg->capacity);
  }
  uint32_t nl = 0;
  for (mk_block_t *b = pg->local_free; b != NULL; b = b->next) nl++;
  CHECK(n + nl + pg->used <= pg->capacity);
  mk_free(p);
}

static void *free_list_of(void *arg) {
  void **v = arg;
  for (int i = 0; v[i]; i++) mk_free(v[i]);
  return NULL;
}

TEST(page_collect_merges_remote_frees) {
  enum { N = 100 };
  void *v[N + 1] = {0};
  for (int i = 0; i < N; i++) v[i] = mk_malloc(272); /* bin of its own in this test */
  mk_page_t *pg = mk_page_of(mk_segment_of(v[0]), v[0]);
  uint32_t used = pg->used;
  int same = 0;
  for (int i = 0; i < N; i++) same += mk_page_of(mk_segment_of(v[i]), v[i]) == pg;
  pthread_t t;
  pthread_create(&t, NULL, free_list_of, v);
  pthread_join(t, NULL);
  CHECK(pg->used == used); /* remote frees are not counted until collected */
  CHECK((atomic_load(&pg->xthread_free) & ~MK_TAG_MASK) != 0);
  mk_page_collect(pg);
  CHECK(pg->used == used - (uint32_t)same);
  CHECK((atomic_load(&pg->xthread_free) & ~MK_TAG_MASK) == 0);
}

TEST(page_full_list_and_delayed_free) {
  mk_heap_t *h = mk_heap_get();
  enum { N = 5000 };
  static void *v[N + 1];
  for (int i = 0; i < N; i++) v[i] = mk_malloc(400); /* 448-byte bin, ~146 per page */
  v[N] = NULL;
  mk_page_t *first = mk_page_of(mk_segment_of(v[0]), v[0]);
  CHECK(first->flags & MK_PAGE_IN_FULL);
  CHECK((atomic_load(&first->xthread_free) & MK_TAG_MASK) == MK_DELAYED_USE);
  void *one[2] = {v[0], NULL};
  pthread_t t;
  pthread_create(&t, NULL, free_list_of, one);
  pthread_join(t, NULL);
  /* the remote free went to the heap, and cleared the tag */
  CHECK(atomic_load(&h->delayed_free) == (mk_block_t *)v[0]);
  CHECK((atomic_load(&first->xthread_free) & MK_TAG_MASK) == MK_DELAYED_NONE);
  mk_heap_delayed_free_all(h);
  CHECK(!(first->flags & MK_PAGE_IN_FULL));
  for (int i = 1; i < N; i++) mk_free(v[i]);
}

TEST(page_last_page_of_bin_is_kept) {
  mk_heap_t *h = mk_heap_get();
  unsigned bin = mk_bin(3000);
  mk_collect(false);
  for (int i = 0; i < 1000; i++) mk_free(mk_malloc(3000));
  CHECK(h->queues[bin].first != NULL); /* no page churn in a malloc/free loop */
  mk_stats_t a, b;
  mk_stats_get(&a);
  for (int i = 0; i < 1000; i++) mk_free(mk_malloc(3000));
  mk_stats_get(&b);
  CHECK(b.pages_allocated == a.pages_allocated);
  mk_collect(false);
  CHECK(h->queues[bin].first == NULL); /* collect gives it back */
}
