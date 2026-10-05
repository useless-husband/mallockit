/* Size classes: every size gets the smallest bin that fits, waste is
 * bounded, and the properties the fast paths rely on hold. */
#include "test.h"

TEST(sizeclass_smallest_fitting_bin) {
  unsigned prev = 1;
  for (size_t n = 0; n <= MK_MEDIUM_MAX; n++) {
    unsigned b = mk_bin(n);
    CHECK(b >= 1 && b <= MK_BIN_LAST);
    CHECK(mk_bin_size(b) >= n);
    if (b > 1) CHECK(mk_bin_size(b - 1) < n); /* no smaller bin would do */
    CHECK(b >= prev);                          /* monotonic */
    prev = b;
  }
  CHECK(mk_bin(MK_SMALL_MAX) == MK_BIN_SMALL_LAST);
  CHECK(mk_bin(MK_MEDIUM_MAX) == MK_BIN_LAST);
  CHECK(mk_bin_size(MK_BIN_LAST) == MK_MEDIUM_MAX);
}

TEST(sizeclass_internal_waste_bound) {
  for (unsigned b = 1; b <= MK_BIN_LAST; b++) CHECK(mk_bin_size(b) % 16 == 0);
  for (size_t n = 1; n <= MK_MEDIUM_MAX; n++) {
    size_t bs = mk_bin_size(mk_bin(n));
    if (n <= 128)
      CHECK(bs - n < 16);
    else
      CHECK((double)(bs - n) / (double)bs < 0.20); /* four bins per doubling */
  }
}

/* heap->direct[] is indexed by (size+15)/16: all sizes sharing an index
 * must share a bin. */
TEST(sizeclass_direct_index_consistent) {
  for (size_t n = 0; n <= MK_DIRECT_MAX; n++) {
    size_t w = (n + 15) / 16;
    size_t lo = w == 0 ? 0 : w * 16 - 15;
    CHECK(mk_bin(n) == mk_bin(lo));
    CHECK(mk_bin(n) == mk_bin(w * 16));
  }
}

/* mk_malloc_aligned relies on: if sz is a multiple of a power of two A,
 * the bin size of sz is a multiple of A too. */
TEST(sizeclass_closed_under_alignment) {
  for (size_t a = 32; a <= MK_MEDIUM_MAX; a <<= 1)
    for (size_t sz = a; sz <= MK_MEDIUM_MAX; sz += a) CHECK(mk_bin_size(mk_bin(sz)) % a == 0);
}

/* Block areas start at multiples of the kind's largest block size. */
TEST(sizeclass_page_start_alignment) {
  size_t hs = mk_segment_header_size(MK_KIND_SMALL), hm = mk_segment_header_size(MK_KIND_MEDIUM);
  CHECK(hs % MK_SMALL_START_ALIGN == 0 && hs < ((size_t)1 << MK_SMALL_PAGE_SHIFT));
  CHECK(hm % MK_MEDIUM_START_ALIGN == 0 && hm < ((size_t)1 << MK_MEDIUM_PAGE_SHIFT));
  CHECK(mk_bin_size(MK_BIN_SMALL_LAST) <= MK_SMALL_START_ALIGN);
  CHECK(mk_bin_size(MK_BIN_LAST) <= MK_MEDIUM_START_ALIGN);
  /* page 0 still holds at least a few of the largest blocks */
  CHECK((((size_t)1 << MK_SMALL_PAGE_SHIFT) - hs) / MK_SMALL_MAX >= 4);
  CHECK((((size_t)1 << MK_MEDIUM_PAGE_SHIFT) - hm) / MK_MEDIUM_MAX >= 4);
}
