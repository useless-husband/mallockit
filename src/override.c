/* Replacing the system allocator (shared library build, MK_OVERRIDE=1).
 *
 * Linux/glibc: define the malloc family; LD_PRELOAD (or linking first) puts
 *   these definitions ahead of glibc's for the whole process, including
 *   glibc's own internal calls and libstdc++'s operator new.
 * macOS: two mechanisms, as mimalloc and jemalloc use:
 *   1. a malloc zone registered at load time and made the default zone,
 *      so malloc_zone_malloc(malloc_default_zone(), ...) (CoreFoundation,
 *      Objective-C, Swift) lands here;
 *   2. dyld interposing (__DATA,__interpose) of malloc, free, ... so that
 *      plain calls skip the zone dispatch. Interposing only takes effect
 *      when the library is loaded with DYLD_INSERT_LIBRARIES (or linked);
 *      DYLD_* variables are stripped for SIP-protected and hardened-runtime
 *      binaries, see docs/DESIGN.md.
 * Pointers that do not belong to mallockit (allocated before the switch,
 * or by another zone) are routed back to the system allocator. */
#include "internal.h"

#include <errno.h>
#include <stdlib.h>

#if MK_OVERRIDE

static void mk_print_stats_at_exit(void) __attribute__((destructor));
static void mk_print_stats_at_exit(void) {
  if (mk_options.print_stats) mk_stats_print();
}

#if defined(__linux__)

#include <malloc.h>

#if defined(__GLIBC__)
extern void __libc_free(void *);
extern void *__libc_realloc(void *, size_t);
#define MK_FOREIGN_FREE(p) __libc_free(p)
#define MK_FOREIGN_REALLOC(p, n) __libc_realloc(p, n)
#else
#define MK_FOREIGN_FREE(p) ((void)(p))
#define MK_FOREIGN_REALLOC(p, n) ((void)(p), (void *)NULL)
#endif

#define MK_EXPORT __attribute__((visibility("default")))

MK_EXPORT void *malloc(size_t n) { return mk_malloc(n); }
MK_EXPORT void *calloc(size_t c, size_t n) { return mk_calloc(c, n); }
MK_EXPORT void free(void *p) {
  if (mk_likely(mk_owns(p)))
    mk_free(p);
  else if (p != NULL)
    MK_FOREIGN_FREE(p);
}
MK_EXPORT void cfree(void *p) { free(p); }
MK_EXPORT void *realloc(void *p, size_t n) {
  if (p == NULL || mk_owns(p)) return mk_realloc(p, n);
  return MK_FOREIGN_REALLOC(p, n);
}
MK_EXPORT void *reallocarray(void *p, size_t c, size_t n) {
  size_t total;
  if (__builtin_mul_overflow(c, n, &total)) {
    errno = ENOMEM;
    return NULL;
  }
  return realloc(p, total);
}
MK_EXPORT int posix_memalign(void **out, size_t a, size_t n) { return mk_posix_memalign(out, a, n); }
MK_EXPORT void *aligned_alloc(size_t a, size_t n) { return mk_aligned_alloc(a, n); }
MK_EXPORT void *memalign(size_t a, size_t n) { return mk_memalign(a, n); }
MK_EXPORT void *valloc(size_t n) { return mk_valloc(n); }
MK_EXPORT void *pvalloc(size_t n) { return mk_pvalloc(n); }
MK_EXPORT size_t malloc_usable_size(void *p) { return mk_owns(p) ? mk_usable_size(p) : 0; }

#elif defined(__APPLE__)

#include <mach/mach.h>
#include <malloc/malloc.h>

/* ------------------------------------------------------------ the zone */

static size_t zone_size(malloc_zone_t *z, const void *p) {
  (void)z;
  return mk_block_usable(p);
}
static void *zone_malloc(malloc_zone_t *z, size_t n) {
  (void)z;
  return mk_malloc(n);
}
static void *zone_calloc(malloc_zone_t *z, size_t c, size_t n) {
  (void)z;
  return mk_calloc(c, n);
}
static void *zone_valloc(malloc_zone_t *z, size_t n) {
  (void)z;
  return mk_valloc(n);
}
static void zone_free(malloc_zone_t *z, void *p) {
  (void)z;
  if (mk_owns(p)) mk_free(p);
}
static void *zone_realloc(malloc_zone_t *z, void *p, size_t n) {
  (void)z;
  return mk_realloc(p, n);
}
static void zone_destroy(malloc_zone_t *z) { (void)z; }
static unsigned zone_batch_malloc(malloc_zone_t *z, size_t n, void **out, unsigned count) {
  (void)z;
  unsigned i = 0;
  for (; i < count; i++) {
    out[i] = mk_malloc(n);
    if (out[i] == NULL) break;
  }
  return i;
}
static void zone_batch_free(malloc_zone_t *z, void **ps, unsigned count) {
  for (unsigned i = 0; i < count; i++) zone_free(z, ps[i]);
}
static void *zone_memalign(malloc_zone_t *z, size_t a, size_t n) {
  (void)z;
  return mk_memalign(a, n);
}
static void zone_free_definite_size(malloc_zone_t *z, void *p, size_t n) {
  (void)n;
  zone_free(z, p);
}
static size_t zone_pressure_relief(malloc_zone_t *z, size_t goal) {
  (void)z;
  (void)goal;
  mk_stats_t before, after;
  mk_stats_get(&before);
  mk_collect(true);
  mk_stats_get(&after);
  return after.purged_bytes - before.purged_bytes;
}
static boolean_t zone_claimed_address(malloc_zone_t *z, void *p) {
  (void)z;
  return mk_owns(p);
}

static kern_return_t intro_enumerator(task_t task, void *ctx, unsigned type_mask, vm_address_t zone_address,
                                      memory_reader_t reader, vm_range_recorder_t recorder) {
  (void)task, (void)ctx, (void)type_mask, (void)zone_address, (void)reader, (void)recorder;
  return KERN_SUCCESS; /* not implemented: leaks(1)/heap(1) see no blocks */
}
static size_t intro_good_size(malloc_zone_t *z, size_t n) {
  (void)z;
  return mk_good_size(n);
}
static boolean_t intro_check(malloc_zone_t *z) {
  (void)z;
  return true;
}
static void intro_print(malloc_zone_t *z, boolean_t verbose) {
  (void)z, (void)verbose;
  mk_stats_print();
}
static void intro_log(malloc_zone_t *z, void *p) { (void)z, (void)p; }
static void intro_force_lock(malloc_zone_t *z) {
  (void)z;
  mk_fork_prepare();
}
static void intro_force_unlock(malloc_zone_t *z) {
  (void)z;
  mk_fork_parent();
}
static void intro_reinit_lock(malloc_zone_t *z) {
  (void)z;
  mk_fork_child();
}
static void intro_statistics(malloc_zone_t *z, malloc_statistics_t *st) {
  (void)z;
  mk_stats_t s;
  mk_stats_get(&s);
  memset(st, 0, sizeof(*st));
  st->size_allocated = s.mapped_bytes;
  st->max_size_in_use = s.peak_mapped_bytes;
}
static boolean_t intro_zone_locked(malloc_zone_t *z) {
  (void)z;
  return false;
}

static malloc_introspection_t mk_introspect;
static malloc_zone_t mk_zone;

static malloc_zone_t *mk_zone_default(void) {
  malloc_zone_t **zones = NULL;
  unsigned count = 0;
  if (malloc_get_all_zones(0, NULL, (vm_address_t **)&zones, &count) != KERN_SUCCESS) count = 0;
  return count > 0 ? zones[0] : malloc_default_zone();
}

/* Run before other initializers of this image, after libSystem's. */
__attribute__((constructor(101))) static void mk_zone_register(void) {
  mk_init();
  mk_introspect.enumerator = intro_enumerator;
  mk_introspect.good_size = intro_good_size;
  mk_introspect.check = intro_check;
  mk_introspect.print = intro_print;
  mk_introspect.log = intro_log;
  mk_introspect.force_lock = intro_force_lock;
  mk_introspect.force_unlock = intro_force_unlock;
  mk_introspect.statistics = intro_statistics;
  mk_introspect.zone_locked = intro_zone_locked;
  mk_introspect.reinit_lock = intro_reinit_lock;

  mk_zone.size = zone_size;
  mk_zone.malloc = zone_malloc;
  mk_zone.calloc = zone_calloc;
  mk_zone.valloc = zone_valloc;
  mk_zone.free = zone_free;
  mk_zone.realloc = zone_realloc;
  mk_zone.destroy = zone_destroy;
  mk_zone.zone_name = "mallockit";
  mk_zone.batch_malloc = zone_batch_malloc;
  mk_zone.batch_free = zone_batch_free;
  mk_zone.introspect = &mk_introspect;
  mk_zone.version = 10; /* up to claimed_address */
  mk_zone.memalign = zone_memalign;
  mk_zone.free_definite_size = zone_free_definite_size;
  mk_zone.pressure_relief = zone_pressure_relief;
  mk_zone.claimed_address = zone_claimed_address;

  /* The purgeable zone is created lazily and borrows the default zone;
   * create it now so that it keeps using the system zone. */
  malloc_zone_t *purgeable = malloc_default_purgeable_zone();
  malloc_zone_register(&mk_zone);
  /* Re-register the current first zone until ours moves to the front
   * (malloc_zone_unregister swaps the last zone into the freed slot). */
  for (int i = 0; i < 64; i++) {
    malloc_zone_t *dflt = mk_zone_default();
    if (dflt == &mk_zone) break;
    malloc_zone_unregister(dflt);
    malloc_zone_register(dflt);
    if (purgeable != NULL) {
      malloc_zone_unregister(purgeable);
      malloc_zone_register(purgeable);
    }
  }
}

/* --------------------------------------------------------- interposing */

static void *mk_ov_malloc(size_t n) { return mk_malloc(n); }
static void *mk_ov_calloc(size_t c, size_t n) { return mk_calloc(c, n); }
static void mk_ov_free(void *p) {
  if (mk_likely(mk_owns(p)))
    mk_free(p);
  else if (p != NULL)
    free(p); /* inside the interposing image this is the system free */
}
static void *mk_ov_realloc(void *p, size_t n) {
  if (p == NULL || mk_owns(p)) return mk_realloc(p, n);
  return realloc(p, n); /* stays with the zone that owns it */
}
static void *mk_ov_reallocf(void *p, size_t n) {
  void *q = mk_ov_realloc(p, n);
  if (q == NULL && p != NULL && n != 0) mk_ov_free(p);
  return q;
}
static void *mk_ov_valloc(size_t n) { return mk_valloc(n); }
static int mk_ov_posix_memalign(void **out, size_t a, size_t n) { return mk_posix_memalign(out, a, n); }
static void *mk_ov_aligned_alloc(size_t a, size_t n) { return mk_aligned_alloc(a, n); }
static size_t mk_ov_malloc_size(const void *p) { return mk_owns(p) ? mk_usable_size(p) : malloc_size(p); }
static size_t mk_ov_malloc_good_size(size_t n) { return mk_good_size(n); }

struct mk_interpose_s {
  const void *replacement;
  const void *target;
};
#define MK_INTERPOSE(repl, target)                                                                                    \
  __attribute__((used)) static const struct mk_interpose_s mk_interpose_##target                                      \
      __attribute__((section("__DATA,__interpose"))) = {(const void *)&repl, (const void *)&target}

MK_INTERPOSE(mk_ov_malloc, malloc);
MK_INTERPOSE(mk_ov_calloc, calloc);
MK_INTERPOSE(mk_ov_free, free);
MK_INTERPOSE(mk_ov_realloc, realloc);
MK_INTERPOSE(mk_ov_reallocf, reallocf);
MK_INTERPOSE(mk_ov_valloc, valloc);
MK_INTERPOSE(mk_ov_posix_memalign, posix_memalign);
MK_INTERPOSE(mk_ov_aligned_alloc, aligned_alloc);
MK_INTERPOSE(mk_ov_malloc_size, malloc_size);
MK_INTERPOSE(mk_ov_malloc_good_size, malloc_good_size);

#if __has_include(<malloc/_malloc_type.h>)
/* Typed allocation entry points (macOS 15+): compilers may emit these
 * instead of malloc/calloc/... */
#include <malloc/_malloc_type.h>
static void *mk_ov_t_malloc(size_t n, malloc_type_id_t t) {
  (void)t;
  return mk_malloc(n);
}
static void *mk_ov_t_calloc(size_t c, size_t n, malloc_type_id_t t) {
  (void)t;
  return mk_calloc(c, n);
}
static void *mk_ov_t_realloc(void *p, size_t n, malloc_type_id_t t) {
  (void)t;
  return mk_ov_realloc(p, n);
}
static void mk_ov_t_free(void *p, malloc_type_id_t t) {
  (void)t;
  mk_ov_free(p);
}
static void *mk_ov_t_valloc(size_t n, malloc_type_id_t t) {
  (void)t;
  return mk_valloc(n);
}
static int mk_ov_t_posix_memalign(void **out, size_t a, size_t n, malloc_type_id_t t) {
  (void)t;
  return mk_posix_memalign(out, a, n);
}
static void *mk_ov_t_aligned_alloc(size_t a, size_t n, malloc_type_id_t t) {
  (void)t;
  return mk_aligned_alloc(a, n);
}
MK_INTERPOSE(mk_ov_t_malloc, malloc_type_malloc);
MK_INTERPOSE(mk_ov_t_calloc, malloc_type_calloc);
MK_INTERPOSE(mk_ov_t_realloc, malloc_type_realloc);
MK_INTERPOSE(mk_ov_t_free, malloc_type_free);
MK_INTERPOSE(mk_ov_t_valloc, malloc_type_valloc);
MK_INTERPOSE(mk_ov_t_posix_memalign, malloc_type_posix_memalign);
MK_INTERPOSE(mk_ov_t_aligned_alloc, malloc_type_aligned_alloc);
#endif

#endif /* __APPLE__ */
#endif /* MK_OVERRIDE */
