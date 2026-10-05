/* Guard mode (built with MK_DEBUG=1, e.g. libmallockit-debug).
 *
 * Block layout, U = usable bytes of the underlying block:
 *   [0, n)        the caller's n bytes
 *   [n, U-8)      canary bytes 0xA5 (at least 8 of them)
 *   [U-8, U)      n XOR a magic word
 * Free blocks of small/medium pages are filled with 0xDF after their first
 * word (the free-list link), and each page keeps one bit per block saying
 * whether it is handed out. That detects:
 *   - buffer overruns (canary or size word changed)        at free
 *   - double frees (bit already clear), also cross-thread    at free
 *   - frees of pointers that are not block starts            at free
 *   - writes after free (0xDF fill changed)                  at reuse
 *   - corrupted free-list links                              at pop */
#include "internal.h"

#include <stdlib.h>

static mk_error_fun *_Atomic mk_err_handler;

void mk_set_error_handler(mk_error_fun *fn) { atomic_store(&mk_err_handler, fn); }

static const char *mk_err_name(int err) {
  switch (err) {
  case MK_ERR_DOUBLE_FREE: return "double free";
  case MK_ERR_OVERRUN: return "buffer overrun (canary damaged)";
  case MK_ERR_INVALID_FREE: return "free of a pointer that is not a block start";
  case MK_ERR_WRITE_AFTER_FREE: return "write after free";
  case MK_ERR_CORRUPT_FREELIST: return "corrupted free list";
  default: return "error";
  }
}

void mk_error(int err, const void *p, const char *msg) {
  mk_error_fun *fn = atomic_load(&mk_err_handler);
  if (fn != NULL) {
    fn(err, p, msg ? msg : mk_err_name(err));
    return;
  }
  char buf[64] = "0x";
  static const char hex[] = "0123456789abcdef";
  uintptr_t a = (uintptr_t)p;
  for (int i = 0; i < 16; i++) buf[2 + i] = hex[(a >> (60 - 4 * i)) & 15];
  buf[18] = 0;
  mk_write_err("mallockit: ");
  mk_write_err(mk_err_name(err));
  mk_write_err(" at ");
  mk_write_err(buf);
  mk_write_err("\n");
  abort();
}

#if MK_DEBUG

#define MK_DBG_MAGIC 0x6d6b2d6775617264ull /* "mk-guard" */
#define MK_CANARY 0xA5
#define MK_FREED 0xDF

size_t mk_debug_padding(void) { return 16; }

static void mk_put_size(void *block, size_t usable, size_t n) {
  uint64_t v = (uint64_t)n ^ MK_DBG_MAGIC;
  memcpy((uint8_t *)block + usable - 8, &v, 8);
}

static size_t mk_get_size(const void *block, size_t usable) {
  uint64_t v;
  memcpy(&v, (const uint8_t *)block + usable - 8, 8);
  return (size_t)(v ^ MK_DBG_MAGIC);
}

static bool mk_has_bits(const mk_segment_t *seg) { return seg->kind != MK_KIND_LARGE; }

static size_t mk_block_index(const mk_page_t *page, const void *p) {
  return (size_t)((const uint8_t *)p - page->start) / page->block_size;
}

void mk_debug_page_init(mk_segment_t *seg, mk_page_t *page) {
  if (!mk_has_bits(seg)) return;
  size_t words = (((size_t)1 << seg->page_shift) / 16 + 63) / 64;
  if (seg->kind == MK_KIND_MEDIUM) words = (((size_t)1 << seg->page_shift) / mk_bin_size(MK_BIN_SMALL_LAST + 1) + 63) / 64;
  for (size_t i = 0; i < words; i++) atomic_store_explicit(&page->alloc_bits[i], 0, memory_order_relaxed);
}

void mk_debug_check_next(mk_page_t *page, mk_block_t *b) {
  uintptr_t lo = (uintptr_t)page->start;
  uintptr_t hi = lo + (uintptr_t)page->capacity * page->block_size;
  uintptr_t a = (uintptr_t)b;
  if (a < lo || a >= hi || (a - lo) % page->block_size != 0) mk_error(MK_ERR_CORRUPT_FREELIST, b, NULL);
}

void *mk_debug_on_alloc(void *block, size_t usable, size_t n) {
  mk_segment_t *seg = mk_segment_of(block);
  mk_page_t *page = mk_page_of(seg, block);
  if (mk_has_bits(seg)) {
    const uint8_t *q = (const uint8_t *)block;
    for (size_t i = sizeof(mk_block_t); i < usable; i++) {
      if (q[i] != MK_FREED) {
        mk_error(MK_ERR_WRITE_AFTER_FREE, block, NULL);
        break;
      }
    }
    size_t idx = mk_block_index(page, block);
    uint64_t bit = (uint64_t)1 << (idx % 64);
    uint64_t old = atomic_fetch_or_explicit(&page->alloc_bits[idx / 64], bit, memory_order_relaxed);
    if (old & bit) mk_error(MK_ERR_CORRUPT_FREELIST, block, "block handed out twice");
  }
  memset((uint8_t *)block + n, MK_CANARY, usable - 8 - n);
  mk_put_size(block, usable, n);
  return block;
}

bool mk_debug_on_free(void *p) {
  if (!mk_owns(p)) {
    mk_error(MK_ERR_INVALID_FREE, p, NULL);
    return false;
  }
  mk_segment_t *seg = mk_segment_of(p);
  mk_page_t *page = mk_page_of(seg, p);
  if (page->in_use == 0 || (uint8_t *)p < page->start) {
    mk_error(MK_ERR_INVALID_FREE, p, NULL);
    return false;
  }
  size_t usable = page->block_size;
  if (seg->kind == MK_KIND_LARGE) {
    if ((uint8_t *)p != page->start) {
      mk_error(MK_ERR_INVALID_FREE, p, NULL);
      return false;
    }
  } else {
    size_t off = (size_t)((uint8_t *)p - page->start);
    if (off % usable != 0 || off / usable >= page->capacity) {
      mk_error(MK_ERR_INVALID_FREE, p, NULL);
      return false;
    }
    size_t idx = off / usable;
    uint64_t bit = (uint64_t)1 << (idx % 64);
    uint64_t old = atomic_fetch_and_explicit(&page->alloc_bits[idx / 64], ~bit, memory_order_relaxed);
    if ((old & bit) == 0) {
      mk_error(MK_ERR_DOUBLE_FREE, p, NULL);
      return false;
    }
  }
  size_t n = mk_get_size(p, usable);
  if (n > usable - 8) {
    mk_error(MK_ERR_OVERRUN, p, "buffer overrun (size word damaged)");
  } else {
    const uint8_t *c = (const uint8_t *)p;
    for (size_t i = n; i < usable - 8; i++) {
      if (c[i] != MK_CANARY) {
        mk_error(MK_ERR_OVERRUN, p, NULL);
        break;
      }
    }
  }
  if (mk_has_bits(seg)) memset((uint8_t *)p + sizeof(mk_block_t), MK_FREED, usable - sizeof(mk_block_t));
  return true;
}

size_t mk_debug_size(const void *p, size_t usable) {
  size_t n = mk_get_size(p, usable);
  return n <= usable - 8 ? n : 0;
}

#endif
