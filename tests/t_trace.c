/* Randomised trace driver. Fixed seeds; on failure the seed is printed and
 * `./build/test_unit trace` reproduces it. Every live allocation is filled
 * with a pattern derived from its slot, so any overlap between blocks, any
 * block handed out twice, and any realloc that loses data shows up as a
 * pattern mismatch. Alignment and usable size are checked on every call,
 * and every few thousand operations all live blocks are sorted by address
 * and checked for overlap of their full usable ranges. */
#include "test.h"

#include <errno.h>

typedef struct {
  uint8_t *p;
  size_t size;
  uint8_t tag;
} slot_t;

static inline uint8_t pat(uint8_t tag, size_t j) { return (uint8_t)(tag + j * 131u + (j >> 9)); }

/* Blocks up to 64 KiB are written and checked byte by byte; bigger ones in
 * their first and last 4 KiB plus one byte per 4 KiB in between (exact
 * overlap is still caught by no_overlap()). */
static inline size_t next_j(size_t j, size_t n) {
  return (n > 65536 && j >= 4096 && j + 4096 < n) ? j + 4096 : j + 1;
}

static void fill(slot_t *s) {
  for (size_t j = 0; j < s->size; j = next_j(j, s->size)) s->p[j] = pat(s->tag, j);
}

/* the first n bytes still hold the pattern written for a block of s->size */
static bool intact(const slot_t *s, size_t n) {
  for (size_t j = 0; j < n; j = next_j(j, s->size))
    if (s->p[j] != pat(s->tag, j)) return false;
  return true;
}

static size_t rand_size(uint64_t *r) {
  unsigned k = (unsigned)(mk_rand(r) % 1000);
  if (k < 700) return mk_rand(r) % 257;                  /* 0..256 */
  if (k < 900) return 257 + mk_rand(r) % (8192 - 256);   /* small */
  if (k < 970) return 8193 + mk_rand(r) % (65536 - 8192); /* medium */
  if (k < 995) return 65537 + mk_rand(r) % (1 << 20);     /* large */
  return (1 << 20) + mk_rand(r) % (7 << 20);              /* large, > 1 segment sometimes */
}

static int cmp_slot(const void *a, const void *b) {
  const slot_t *x = a, *y = b;
  return x->p < y->p ? -1 : x->p > y->p;
}

static bool no_overlap(slot_t *live, size_t n) {
  qsort(live, n, sizeof(slot_t), cmp_slot);
  for (size_t i = 0; i + 1 < n; i++)
    if (live[i].p + mk_usable_size(live[i].p) > live[i + 1].p) return false;
  return true;
}

static void run_trace(uint64_t seed, long ops, size_t nslots) {
  mk_test_seed = seed;
  uint64_t r = seed * 0x9E3779B97F4A7C15ull + 1;
  slot_t *slots = calloc(nslots, sizeof(slot_t));
  slot_t *scratch = calloc(nslots, sizeof(slot_t));
  for (long op = 0; op < ops; op++) {
    slot_t *s = &slots[mk_rand(&r) % nslots];
    unsigned what = (unsigned)(mk_rand(&r) % 100);
    if (s->p == NULL) {
      size_t n = rand_size(&r), align = 16;
      unsigned api = (unsigned)(mk_rand(&r) % 10);
      if (api <= 5) {
        s->p = mk_malloc(n);
      } else if (api == 6) {
        s->p = mk_calloc(1 + n / 8, 8);
        n = (1 + n / 8) * 8;
        CHECK(s->p != NULL);
        for (size_t j = 0; j < n; j += (n > 65536 ? 4093 : 1)) CHECK(s->p[j] == 0);
      } else if (api == 7 || api == 8) {
        unsigned lg = 3 + (unsigned)(mk_rand(&r) % 12); /* 8 .. 16 KiB */
        if (mk_rand(&r) % 50 == 0) lg = 15 + (unsigned)(mk_rand(&r) % 9); /* up to 4 MiB */
        align = (size_t)1 << lg;
        if (api == 7) {
          void *q = NULL;
          CHECK(mk_posix_memalign(&q, align, n) == 0);
          s->p = q;
        } else {
          s->p = mk_aligned_alloc(align, n);
        }
      } else {
        s->p = mk_realloc(NULL, n);
      }
      CHECK(s->p != NULL);
      CHECK(((uintptr_t)s->p & ((align < 16 ? 16 : align) - 1)) == 0);
      CHECK(mk_usable_size(s->p) >= n);
      s->size = n;
      s->tag = (uint8_t)mk_rand(&r);
      fill(s);
    } else if (what < 30) {
      size_t n = rand_size(&r);
      size_t keep = s->size < n ? s->size : n;
      uint8_t *q = mk_realloc(s->p, n);
      CHECK(q != NULL);
      s->p = q;
      CHECK(intact(s, keep)); /* realloc preserved the common prefix */
      s->size = n;
      CHECK(((uintptr_t)q & 15) == 0 && mk_usable_size(q) >= n);
      fill(s);
    } else {
      CHECK(intact(s, s->size)); /* nobody else wrote into this block */
      mk_free(s->p);
      s->p = NULL;
    }
    if (op % 4096 == 4095) {
      size_t n = 0;
      for (size_t i = 0; i < nslots; i++)
        if (slots[i].p) scratch[n++] = slots[i];
      CHECK(no_overlap(scratch, n));
    }
  }
  for (size_t i = 0; i < nslots; i++) {
    if (slots[i].p) {
      CHECK(intact(&slots[i], slots[i].size));
      mk_free(slots[i].p);
    }
  }
  free(slots);
  free(scratch);
}

TEST(trace_random_seeds) {
  long ops = 120000 / mk_test_scale();
  const char *e = getenv("MK_TRACE_SEEDS"); /* more seeds on demand */
  uint64_t nseeds = e ? (uint64_t)atoi(e) : 6;
  for (uint64_t seed = 1; seed <= nseeds && !mk_test_current_failed; seed++) run_trace(seed, ops, 1500);
}

TEST(trace_many_live_small_blocks) {
  run_trace(1001, 200000 / mk_test_scale(), 20000);
}

/* Same driver with the purge delay at 0: every page and segment that
 * empties is handed back to the OS at once, so reuse after purge is hit
 * constantly. */
TEST(trace_with_immediate_purge) {
  long saved = mk_options.purge_delay_ms;
  mk_options.purge_delay_ms = 0;
  run_trace(77, 60000 / mk_test_scale(), 800);
  mk_options.purge_delay_ms = saved;
}
