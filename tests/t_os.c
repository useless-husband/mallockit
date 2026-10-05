/* OS layer: page size, aligned mappings, purge/reuse, in-place extension. */
#include "test.h"

#include <unistd.h>

TEST(os_page_size_is_queried) {
  mk_free(mk_malloc(1)); /* initialises */
  CHECK(mk_os_page_size == (size_t)sysconf(_SC_PAGESIZE));
  CHECK(mk_is_pow2(mk_os_page_size));
#if defined(__APPLE__) && defined(__aarch64__)
  CHECK(mk_os_page_size == 16384); /* Apple Silicon */
#endif
}

TEST(os_aligned_mappings) {
  for (size_t align = mk_os_page_size; align <= ((size_t)32 << 20); align <<= 2) {
    size_t size = 3 * mk_os_page_size;
    uint8_t *p = mk_os_alloc_aligned(size, align);
    CHECK(p != NULL);
    CHECK(((uintptr_t)p & (align - 1)) == 0);
    for (size_t i = 0; i < size; i += 512) CHECK(p[i] == 0); /* fresh pages are zero */
    p[size - 1] = 1;
    mk_os_free(p, size);
    uint8_t *q = mk_os_alloc_aligned_at(size, align, mk_os_page_size);
    CHECK(q != NULL && (((uintptr_t)q + mk_os_page_size) & (align - 1)) == 0);
    mk_os_free(q, size);
  }
}

TEST(os_purge_and_reuse) {
  size_t size = 8 * mk_os_page_size;
  uint8_t *p = mk_os_alloc_aligned(size, mk_os_page_size);
  CHECK(p != NULL);
  memset(p, 7, size);
  mk_stats_t a, b;
  mk_stats_get(&a);
  mk_os_purge(p + 1, size - 2); /* shrinks inward to whole pages */
  mk_stats_get(&b);
  CHECK(b.purged_bytes - a.purged_bytes == size - 2 * mk_os_page_size);
  CHECK(p[0] == 7 && p[size - 1] == 7); /* the partial pages were kept */
#if defined(__linux__)
  CHECK(p[mk_os_page_size] == 0); /* MADV_DONTNEED: zero-fill on next touch */
#endif
  mk_os_reuse(p, size);
  memset(p, 9, size);
  CHECK(p[size / 2] == 9);
  mk_os_free(p, size);
}

TEST(os_extend_in_place_never_clobbers) {
  size_t size = 4 * mk_os_page_size;
  uint8_t *p = mk_os_alloc_aligned(size + 4 * mk_os_page_size, mk_os_page_size);
  CHECK(p != NULL);
  /* the range right after [p, p+size) is mapped already: must refuse */
  memset(p + size, 0x33, mk_os_page_size);
  CHECK(!mk_os_try_extend(p + size, mk_os_page_size));
  CHECK(p[size] == 0x33);
  mk_os_free(p, size + 4 * mk_os_page_size);
  /* after the end of a lone mapping it may or may not succeed */
  uint8_t *q = mk_os_alloc_aligned(size, mk_os_page_size);
  if (mk_os_try_extend(q + size, size)) {
    q[2 * size - 1] = 1;
    mk_os_free(q, 2 * size);
  } else {
    mk_os_free(q, size);
  }
}

TEST(os_clock_is_monotonic) {
  uint64_t a = mk_clock_ms(), b = mk_clock_ms();
  CHECK(b >= a);
}
