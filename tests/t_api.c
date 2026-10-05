/* The C API contract: return values, errno, zeroing, alignment, realloc. */
#include "test.h"

#include <errno.h>
#include <stdint.h>

TEST(api_malloc_zero_and_free_null) {
  void *a = mk_malloc(0), *b = mk_malloc(0);
  CHECK(a != NULL && b != NULL && a != b);
  CHECK(((uintptr_t)a & 15) == 0);
  CHECK(mk_usable_size(a) >= 1);
  mk_free(a);
  mk_free(b);
  mk_free(NULL);
  CHECK(mk_usable_size(NULL) == 0);
}

TEST(api_sizes_are_usable_and_aligned) {
  size_t sizes[] = {1, 7, 8, 15, 16, 17, 100, 128, 129, 1000, 1024, 1025, 4096, 8191, 8192, 8193,
                    40000, 65536, 65537, 100000, 1 << 20, (1 << 22) - 1000, 1 << 22, 5 << 20, 33 << 20};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    size_t n = sizes[i];
    uint8_t *p = mk_malloc(n);
    CHECK(p != NULL);
    CHECK(((uintptr_t)p & 15) == 0);
    CHECK(mk_usable_size(p) >= n);
    CHECK(mk_owns(p));
    memset(p, 0x5a, mk_usable_size(p)); /* the whole usable size is writable */
    CHECK(p[0] == 0x5a && p[n - 1] == 0x5a);
    mk_free(p);
  }
}

TEST(api_calloc_zeroes_recycled_memory) {
  for (size_t n = 8; n <= (1u << 21); n *= 3) {
    uint8_t *p = mk_malloc(n);
    CHECK(p != NULL);
    memset(p, 0xff, n);
    mk_free(p);
    uint8_t *q = mk_calloc(1, n);
    CHECK(q != NULL);
    for (size_t i = 0; i < n; i += 61) CHECK(q[i] == 0);
    CHECK(q[n - 1] == 0);
    mk_free(q);
  }
}

TEST(api_overflow_and_huge_requests_fail_cleanly) {
  errno = 0;
  CHECK(mk_calloc(SIZE_MAX / 2, 3) == NULL);
  CHECK(errno == ENOMEM);
  errno = 0;
  CHECK(mk_malloc(SIZE_MAX) == NULL);
  CHECK(errno == ENOMEM);
  errno = 0;
  CHECK(mk_malloc(SIZE_MAX - 4096) == NULL);
  CHECK(errno == ENOMEM);
  void *p = mk_malloc(64);
  errno = 0;
  CHECK(mk_realloc(p, SIZE_MAX - 100) == NULL); /* p stays valid */
  CHECK(errno == ENOMEM);
  CHECK(mk_usable_size(p) >= 64);
  mk_free(p);
  void *q = (void *)1;
  CHECK(mk_posix_memalign(&q, 64, SIZE_MAX - 10) == ENOMEM);
  CHECK(q == (void *)1); /* untouched on failure */
}

TEST(api_alignment_contracts) {
  void *p = NULL;
  CHECK(mk_posix_memalign(&p, 3, 10) == EINVAL);
  CHECK(mk_posix_memalign(&p, 0, 10) == EINVAL);
  CHECK(mk_posix_memalign(&p, sizeof(void *) / 2, 10) == EINVAL);
  errno = 0;
  CHECK(mk_aligned_alloc(48, 96) == NULL && errno == EINVAL);
  void *m = mk_memalign(48, 10); /* rounded up to 64 like glibc */
  CHECK(m != NULL && ((uintptr_t)m & 63) == 0);
  mk_free(m);
  for (size_t a = 8; a <= ((size_t)64 << 20); a <<= 1) {
    size_t sizes[] = {1, a / 2 + 1, a, a + 1, 3 * a + 5, 100000};
    for (size_t j = 0; j < 6; j++) {
      void *q = NULL;
      CHECK(mk_posix_memalign(&q, a, sizes[j]) == 0);
      CHECK(((uintptr_t)q & (a - 1)) == 0);
      CHECK(mk_usable_size(q) >= sizes[j]);
      CHECK(mk_owns(q));
      memset(q, 1, sizes[j]);
      mk_free(q);
    }
  }
  void *v = mk_valloc(10);
  CHECK(v != NULL && ((uintptr_t)v % mk_os_page_size) == 0);
  mk_free(v);
  void *pv = mk_pvalloc(mk_os_page_size + 1);
  CHECK(pv != NULL && ((uintptr_t)pv % mk_os_page_size) == 0 && mk_usable_size(pv) >= 2 * mk_os_page_size);
  mk_free(pv);
}

TEST(api_realloc_keeps_contents) {
  uint8_t *p = mk_realloc(NULL, 10);
  CHECK(p != NULL);
  for (int i = 0; i < 10; i++) p[i] = (uint8_t)(i * 7 % 251);
  size_t sizes[] = {20, 200, 3000, 70000, 1 << 20, 9 << 20, 100, 9, 17 << 20, 64};
  size_t keep = 10;
  for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
    p = mk_realloc(p, sizes[s]);
    CHECK(p != NULL);
    size_t check = keep < sizes[s] ? keep : sizes[s];
    for (size_t i = 0; i < check; i++) CHECK(p[i] == (uint8_t)(i * 7 % 251));
    for (size_t i = 0; i < sizes[s]; i++) p[i] = (uint8_t)(i * 7 % 251);
    keep = sizes[s];
  }
  mk_free(p);
  void *z = mk_malloc(100);
  void *r = mk_realloc(z, 0); /* frees z and returns a minimal block */
  CHECK(r != NULL);
  mk_free(r);
}

TEST(api_realloc_in_place_when_it_fits) {
  void *p = mk_malloc(1000); /* 1024-byte block */
  CHECK(mk_realloc(p, 1020) == p);
  CHECK(mk_realloc(p, 600) == p);
  void *q = mk_realloc(p, 100); /* too much waste: moves */
  CHECK(q != NULL);
  mk_free(q);
  /* large objects grow in place inside their segment */
  mk_stats_t a, b;
  void *l = mk_malloc(200000);
  mk_stats_get(&a);
  void *l2 = mk_realloc(l, 1500000);
  mk_stats_get(&b);
  CHECK(l2 == l);
  CHECK(b.realloc_in_place == a.realloc_in_place + 1);
  void *l3 = mk_realloc(l2, 300000); /* shrink in place */
  CHECK(l3 == l2);
  mk_free(l3);
}

TEST(api_good_size_matches_usable) {
  for (size_t n = 1; n < 70000; n = n * 5 / 4 + 1) {
    void *p = mk_malloc(n);
    CHECK(mk_good_size(n) == mk_usable_size(p));
    mk_free(p);
  }
}

TEST(api_owns_rejects_foreign_pointers) {
  int on_stack;
  static int global;
  void *sys = malloc(32); /* the C library's malloc, not ours */
  CHECK(!mk_owns(NULL));
  CHECK(!mk_owns(&on_stack));
  CHECK(!mk_owns(&global));
  CHECK(!mk_owns(sys));
  CHECK(!mk_owns((void *)(uintptr_t)0xffff000000001000ull));
  free(sys);
  void *p = mk_malloc(32);
  CHECK(mk_owns(p));
  CHECK(mk_block_usable(p) >= 32);
  CHECK(mk_block_usable((uint8_t *)p + 8) == 0); /* interior pointer */
  mk_free(p);
}
