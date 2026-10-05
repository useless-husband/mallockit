/* OS layer: virtual memory, the page size, a millisecond clock, options.
 * Nothing here allocates with malloc: it may run inside malloc itself. */
#include "internal.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

mk_options_t mk_options = {.purge_delay_ms = 10, .print_stats = false, .tsd_direct = false};
size_t mk_os_page_size = 4096;
mk_gstats_t mk_gstats;

static long mk_env_long(const char *name, long dflt) {
  const char *s = getenv(name);
  if (s == NULL || *s == 0) return dflt;
  char *end;
  long v = strtol(s, &end, 10);
  return (*end == 0) ? v : dflt;
}

void mk_os_init(void) {
  /* Query, do not assume: Apple Silicon uses 16 KiB pages, most Linux
   * systems 4 KiB, some arm64 Linux kernels 16 or 64 KiB. */
  long ps = sysconf(_SC_PAGESIZE);
  if (ps > 0 && mk_is_pow2((size_t)ps)) mk_os_page_size = (size_t)ps;
  mk_options.purge_delay_ms = mk_env_long("MALLOCKIT_PURGE_DELAY", 10);
  mk_options.print_stats = mk_env_long("MALLOCKIT_STATS", 0) != 0;
}

static void mk_stat_mapped(ptrdiff_t delta) {
  if (delta >= 0) {
    size_t now = atomic_fetch_add_explicit(&mk_gstats.mapped, (size_t)delta, memory_order_relaxed) + (size_t)delta;
    size_t peak = atomic_load_explicit(&mk_gstats.peak_mapped, memory_order_relaxed);
    while (now > peak &&
           !atomic_compare_exchange_weak_explicit(&mk_gstats.peak_mapped, &peak, now, memory_order_relaxed,
                                                  memory_order_relaxed)) {
    }
  } else {
    atomic_fetch_sub_explicit(&mk_gstats.mapped, (size_t)(-delta), memory_order_relaxed);
  }
}

static void *mk_mmap(void *hint, size_t size) {
  int flags = MAP_PRIVATE | MAP_ANON;
  int fd = -1;
#if defined(__APPLE__)
  fd = (100 << 24); /* VM_MAKE_TAG(100): shows up as its own row in vmmap */
#endif
  void *p = mmap(hint, size, PROT_READ | PROT_WRITE, flags, fd, 0);
  mk_stat_add(mmap_calls, 1);
  return p == MAP_FAILED ? NULL : p;
}

static void mk_munmap(void *p, size_t size) {
  if (size == 0) return;
  munmap(p, size);
  mk_stat_add(munmap_calls, 1);
}

/* Returns q with (q + offset) % align == 0. size, align and offset are
 * multiples of the OS page size; align is a power of two and offset <
 * align. Tries a plain mapping first (often already aligned), otherwise
 * maps size+align bytes and trims both ends. */
void *mk_os_alloc_aligned_at(size_t size, size_t align, size_t offset) {
  if (size == 0 || size > ((size_t)1 << MK_VA_BITS) || align > ((size_t)1 << MK_VA_BITS)) {
    errno = ENOMEM;
    return NULL;
  }
  void *p = mk_mmap(NULL, size);
  if (p == NULL) return NULL;
  if ((((uintptr_t)p + offset) & (align - 1)) != 0) {
    mk_munmap(p, size);
    size_t over = size + align;
    uint8_t *raw = mk_mmap(NULL, over);
    if (raw == NULL) return NULL;
    uint8_t *aligned = (uint8_t *)(mk_align_up((uintptr_t)raw + offset, align) - offset);
    size_t pre = (size_t)(aligned - raw);
    size_t post = over - pre - size;
    mk_munmap(raw, pre);
    mk_munmap(aligned + size, post);
    p = aligned;
  }
  mk_stat_mapped((ptrdiff_t)size);
  return p;
}

void *mk_os_alloc_aligned(size_t size, size_t align) { return mk_os_alloc_aligned_at(size, align, 0); }

void mk_os_free(void *p, size_t size) {
  mk_munmap(p, size);
  mk_stat_mapped(-(ptrdiff_t)size);
}

/* Give the physical pages of [p, p+size) back. The range is shrunk to whole
 * OS pages. Linux: MADV_DONTNEED drops them at once (reads return zero).
 * macOS: MADV_FREE_REUSABLE drops them from the footprint at once, like
 * the system allocator does; mk_os_reuse must be called before reuse. */
void mk_os_purge(void *p, size_t size) {
  uintptr_t start = mk_align_up((uintptr_t)p, mk_os_page_size);
  uintptr_t end = mk_align_down((uintptr_t)p + size, mk_os_page_size);
  if (end <= start) return;
#if defined(__APPLE__)
  int r = madvise((void *)start, end - start, MADV_FREE_REUSABLE);
#else
  int r = madvise((void *)start, end - start, MADV_DONTNEED);
#endif
  (void)r;
  mk_stat_add(madvise_calls, 1);
  mk_stat_add(purged, end - start);
}

void mk_os_reuse(void *p, size_t size) {
#if defined(__APPLE__)
  uintptr_t start = mk_align_up((uintptr_t)p, mk_os_page_size);
  uintptr_t end = mk_align_down((uintptr_t)p + size, mk_os_page_size);
  if (end <= start) return;
  madvise((void *)start, end - start, MADV_FREE_REUSE);
  mk_stat_add(madvise_calls, 1);
#else
  (void)p;
  (void)size;
#endif
}

/* Try to map [end, end+extra) so that an existing mapping can grow in
 * place. Uses the address only as a hint (never MAP_FIXED, which would
 * silently replace whatever lives there) and gives up if the kernel picks
 * another address. */
bool mk_os_try_extend(void *end, size_t extra) {
  void *p = mk_mmap(end, extra);
  if (p == NULL) return false;
  if (p != end) {
    mk_munmap(p, extra);
    return false;
  }
  mk_stat_mapped((ptrdiff_t)extra);
  return true;
}

uint64_t mk_clock_ms(void) {
#if defined(__APPLE__)
  return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000u;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

void mk_write_err(const char *s) {
  size_t n = strlen(s);
  while (n > 0) {
    ssize_t w = write(2, s, n);
    if (w <= 0) return;
    s += w;
    n -= (size_t)w;
  }
}

char *mk_fmt_u64(char *buf, uint64_t v) {
  char tmp[24];
  int n = 0;
  do {
    tmp[n++] = (char)('0' + v % 10);
    v /= 10;
  } while (v != 0);
  while (n > 0) *buf++ = tmp[--n];
  *buf = 0;
  return buf;
}
