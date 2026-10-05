/* mallockit: a thread-caching general-purpose memory allocator.
 *
 * The mk_* functions are always available. The shared library build
 * (libmallockit.so / libmallockit.dylib) additionally replaces the C
 * library's malloc family, see docs/DESIGN.md section "Integration". */
#ifndef MALLOCKIT_H
#define MALLOCKIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void *mk_malloc(size_t size);
void *mk_calloc(size_t count, size_t size);
void *mk_realloc(void *p, size_t size);
void mk_free(void *p);
int mk_posix_memalign(void **out, size_t alignment, size_t size);
void *mk_aligned_alloc(size_t alignment, size_t size);
void *mk_memalign(size_t alignment, size_t size);
void *mk_valloc(size_t size);
void *mk_pvalloc(size_t size);

/* Bytes usable at p (at least the requested size). 0 for NULL. */
size_t mk_usable_size(const void *p);
/* The size mk_malloc(size) would really reserve. */
size_t mk_good_size(size_t size);
/* True if p is the start of a live-or-free block inside memory that
 * mallockit mapped. Used to route foreign pointers when overriding. */
bool mk_owns(const void *p);

/* Give unused memory back: collects this thread's heap and the heaps of
 * exited threads. With force, also purges every free page immediately and
 * unmaps the segment cache. */
void mk_collect(bool force);
/* Explicitly release the calling thread's heap (normally done by a
 * pthread key destructor when the thread exits). */
void mk_thread_done(void);

typedef struct mk_stats_s {
  size_t mapped_bytes;      /* currently mapped from the OS */
  size_t peak_mapped_bytes; /* high-water mark of mapped_bytes */
  size_t segments_in_use;   /* small/medium segments owned by heaps */
  size_t large_in_use;      /* live large objects (one segment each) */
  size_t cached_segments;   /* empty segments kept for reuse */
  size_t purged_bytes;      /* cumulative bytes handed back with madvise */
  uint64_t mmap_calls, munmap_calls, madvise_calls;
  uint64_t pages_allocated, pages_retired;
  uint64_t heaps_created, heaps_abandoned, heaps_adopted;
  uint64_t abandoned_heaps; /* currently waiting for adoption */
  uint64_t remote_frees;    /* blocks freed by a thread other than the owner */
  uint64_t delayed_frees;   /* remote frees routed through the owner heap */
  uint64_t realloc_in_place, realloc_moved; /* large objects only */
} mk_stats_t;

void mk_stats_get(mk_stats_t *out);
void mk_stats_print(void); /* human-readable, to stderr */

/* Guard/debug build (MK_DEBUG=1) error reporting. The default handler
 * prints a message and aborts; tests install their own. */
enum {
  MK_ERR_DOUBLE_FREE = 1,
  MK_ERR_OVERRUN = 2,
  MK_ERR_INVALID_FREE = 3,
  MK_ERR_WRITE_AFTER_FREE = 4,
  MK_ERR_CORRUPT_FREELIST = 5,
};
typedef void mk_error_fun(int err, const void *p, const char *msg);
void mk_set_error_handler(mk_error_fun *fn);
bool mk_is_debug_build(void);

#ifdef __cplusplus
}
#endif
#endif
