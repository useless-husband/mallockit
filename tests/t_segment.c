/* Segment layer: the segment map, page carving, large objects, the cache,
 * and the purge policy. */
#include "test.h"

TEST(segment_lookup_by_masking) {
  for (size_t n = 16; n <= MK_MEDIUM_MAX; n *= 2) {
    uint8_t *p = mk_malloc(n);
    mk_segment_t *s = mk_segment_of(p);
    CHECK(((uintptr_t)s & MK_SEGMENT_MASK) == 0);
    CHECK(s->kind == (n <= MK_SMALL_MAX ? MK_KIND_SMALL : MK_KIND_MEDIUM));
    mk_page_t *pg = mk_page_of(s, p);
    CHECK(pg->in_use && pg->block_size == mk_bin_size(mk_bin(n)));
    CHECK(p >= pg->start && (size_t)(p - pg->start) % pg->block_size == 0);
    CHECK(atomic_load(&s->thread_id) == mk_thread_id());
    CHECK(s->heap == mk_heap_get());
    mk_free(p);
  }
}

/* Fill several pages of a segment and check their areas are disjoint and
 * the free-page mask agrees with used_pages. */
TEST(segment_pages_are_disjoint) {
  enum { N = 3000 };
  static void *v[N];
  for (int i = 0; i < N; i++) v[i] = mk_malloc(2000 + (size_t)(i % 7) * 900); /* several small bins */
  mk_segment_t *s = mk_segment_of(v[0]);
  unsigned used = 0;
  for (unsigned i = 0; i < s->page_count; i++) {
    bool free_bit = (s->free_mask >> i) & 1;
    CHECK(free_bit == !s->pages[i].in_use);
    if (!s->pages[i].in_use) continue;
    used++;
    uint8_t *a0 = s->pages[i].start, *a1 = a0 + (size_t)s->pages[i].reserved * s->pages[i].block_size;
    CHECK(a1 <= (uint8_t *)s + ((size_t)(i + 1) << s->page_shift));
    CHECK(a0 >= (uint8_t *)s + ((size_t)i << s->page_shift));
    if (i == 0) CHECK(a0 >= (uint8_t *)s + mk_segment_header_size(MK_KIND_SMALL));
  }
  CHECK(used == s->used_pages);
  for (int i = 0; i < N; i++) mk_free(v[i]);
}

TEST(segment_large_objects_and_cache) {
  mk_collect(true);
  mk_stats_t a, b, c;
  mk_stats_get(&a);
  void *p = mk_malloc(1 << 20);
  mk_segment_t *s = mk_segment_of(p);
  CHECK(s->kind == MK_KIND_LARGE && s->size == MK_SEGMENT_SIZE);
  CHECK(atomic_load(&s->thread_id) == 0); /* large objects have no owner */
  mk_free(p);
  mk_stats_get(&b);
  CHECK(b.large_in_use == a.large_in_use && b.cached_segments == a.cached_segments + 1);
  void *q = mk_malloc(1 << 20); /* comes from the cache: no new mapping */
  mk_stats_get(&c);
  CHECK(c.mmap_calls == b.mmap_calls);
  mk_free(q);
  void *h = mk_malloc(20 << 20); /* bigger than a segment: exact mapping */
  CHECK(mk_segment_of(h)->size >= (20 << 20));
  mk_free(h);
  mk_stats_get(&c);
  CHECK(c.large_in_use == a.large_in_use);
  mk_collect(true);
  mk_stats_get(&c);
  CHECK(c.cached_segments == 0);
}

TEST(segment_huge_alignment_layout) {
  void *p = NULL;
  CHECK(mk_posix_memalign(&p, (size_t)16 << 20, 1000) == 0);
  CHECK(((uintptr_t)p & ((16 << 20) - 1)) == 0);
  mk_segment_t *s = mk_segment_of(p);
  CHECK((uint8_t *)s == (uint8_t *)p - MK_SEGMENT_SIZE); /* header one segment before */
  CHECK(s->kind == MK_KIND_LARGE && s->pages[0].start == p);
  CHECK(mk_owns(p) && mk_block_usable(p) >= 1000);
  CHECK(!mk_owns((uint8_t *)p + MK_SEGMENT_SIZE));
  memset(p, 1, 1000);
  mk_free(p);
}

/* Free pages are purged after the delay; mk_collect(true) purges now and
 * unmaps everything that is empty. */
TEST(segment_purge_policy) {
  mk_free(mk_malloc(1)); /* the heap itself is mapped on first use */
  mk_collect(true);
  mk_stats_t a, b, c;
  mk_stats_get(&a);
  enum { N = 20000 };
  static void *v[N];
  for (int i = 0; i < N; i++) v[i] = mk_malloc(200);
  for (int i = 0; i < N; i += 2) mk_free(v[i]); /* pages stay partly used */
  mk_stats_get(&b);
  CHECK(b.segments_in_use >= 1);
  for (int i = 1; i < N; i += 2) mk_free(v[i]);
  mk_collect(true);
  mk_stats_get(&c);
  CHECK(c.segments_in_use == 0);
  CHECK(c.purged_bytes > a.purged_bytes || c.munmap_calls > a.munmap_calls);
  CHECK(c.mapped_bytes <= a.mapped_bytes);
}

TEST(segment_delay_zero_purges_on_free) {
  long saved = mk_options.purge_delay_ms;
  mk_options.purge_delay_ms = 0;
  mk_stats_t a, b;
  enum { N = 5000 };
  static void *v[N];
  for (int i = 0; i < N; i++) v[i] = mk_malloc(1000); /* ~80 pages */
  mk_stats_get(&a);
  for (int i = 0; i < N; i++) mk_free(v[i]);
  mk_stats_get(&b);
  mk_options.purge_delay_ms = saved;
  CHECK(b.madvise_calls > a.madvise_calls);
  CHECK(b.purged_bytes - a.purged_bytes >= (size_t)N * 1000 / 2);
}
