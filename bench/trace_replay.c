/* Replays an allocation trace with the process's malloc (pick the allocator
 * with LD_PRELOAD / DYLD_INSERT_LIBRARIES), in the spirit of the MIT 6.172
 * project 3 driver.
 *
 * Trace format (text): lines "a <id> <size>", "r <id> <size>", "f <id>";
 * lines starting with '#' are comments; the first line "# ids <n>" gives
 * the id count.
 *
 *   trace_replay time <trace> [min_seconds]
 *       replays the trace repeatedly (each pass frees everything) without
 *       touching the payload, prints {"ops":..,"seconds":..}.
 *   trace_replay util <trace>
 *       replays once, writing every payload byte (so it is resident), and
 *       prints the peak live payload and the growth of the peak resident
 *       set size during the replay. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <libproc.h>
#endif

typedef struct {
  char op;
  uint32_t id;
  uint64_t size;
} op_t;

static op_t *ops;
static size_t nops, nids;

/* Two passes (count, then parse into an exactly sized array) so that
 * loading leaves no transient peak that would hide the replay's own. */
static void load(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) {
    perror(path);
    exit(2);
  }
  char line[256];
  size_t cap = 0;
  while (fgets(line, sizeof line, f))
    if (line[0] != '#') cap++;
  rewind(f);
  ops = calloc(cap ? cap : 1, sizeof(op_t));
  while (fgets(line, sizeof line, f)) {
    if (line[0] == '#') {
      unsigned long n;
      if (sscanf(line, "# ids %lu", &n) == 1) nids = n;
      continue;
    }
    op_t o = {0};
    unsigned long id = 0;
    unsigned long long sz = 0;
    if (sscanf(line, "%c %lu %llu", &o.op, &id, &sz) < 2) continue;
    o.id = (uint32_t)id;
    o.size = sz;
    if (o.id >= nids) nids = o.id + 1;
    if (nops < cap) ops[nops++] = o;
  }
  fclose(f);
}

static double now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Memory in use, and its peak so far. macOS: the physical footprint (the
 * resident set there still counts pages an allocator handed back with
 * MADV_FREE_REUSABLE). Linux: the resident set. */
static void mem_now_peak(uint64_t *now_b, uint64_t *peak_b) {
#if defined(__APPLE__)
  struct rusage_info_v4 ri;
  if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t *)&ri) == 0) {
    *now_b = ri.ri_phys_footprint;
    *peak_b = ri.ri_lifetime_max_phys_footprint;
    return;
  }
#endif
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
  *peak_b = (uint64_t)ru.ru_maxrss;
#else
  *peak_b = (uint64_t)ru.ru_maxrss * 1024;
#endif
  *now_b = *peak_b;
}

static int replay(void **ptr, uint64_t *sizes, int touch, uint64_t *peak_payload) {
  uint64_t live = 0, peak = 0;
  for (size_t i = 0; i < nops; i++) {
    op_t o = ops[i];
    switch (o.op) {
    case 'a':
      ptr[o.id] = malloc(o.size);
      if (!ptr[o.id] && o.size) return -1;
      if (touch) memset(ptr[o.id], (int)(o.id & 0xff), o.size);
      live += o.size;
      sizes[o.id] = o.size;
      break;
    case 'r': {
      void *q = realloc(ptr[o.id], o.size);
      if (!q && o.size) return -1;
      if (touch && o.size > sizes[o.id]) memset((char *)q + sizes[o.id], 1, o.size - sizes[o.id]);
      ptr[o.id] = q;
      live = live - sizes[o.id] + o.size;
      sizes[o.id] = o.size;
      break;
    }
    case 'f':
      free(ptr[o.id]);
      ptr[o.id] = NULL;
      live -= sizes[o.id];
      sizes[o.id] = 0;
      break;
    }
    if (live > peak) peak = live;
  }
  for (size_t i = 0; i < nids; i++) {
    free(ptr[i]);
    ptr[i] = NULL;
    sizes[i] = 0;
  }
  if (peak_payload) *peak_payload = peak;
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s time|util <trace> [min_seconds]\n", argv[0]);
    return 2;
  }
  load(argv[2]);
  void **ptr = calloc(nids, sizeof(void *));
  uint64_t *sizes = calloc(nids, sizeof(uint64_t));
  /* make the driver's own tables resident before the baseline */
  memset(ptr, 0, nids * sizeof(void *));
  memset(sizes, 0, nids * sizeof(uint64_t));
  if (strcmp(argv[1], "util") == 0) {
    uint64_t base, base_peak, end, end_peak, peak = 0;
    mem_now_peak(&base, &base_peak);
    if (replay(ptr, sizes, 1, &peak) != 0) return 1;
    mem_now_peak(&end, &end_peak);
    uint64_t grown = end_peak > base ? end_peak - base : 0;
    printf("{\"mode\":\"util\",\"ops\":%zu,\"peak_payload\":%llu,\"rss_growth\":%llu,\"util\":%.4f}\n", nops,
           (unsigned long long)peak, (unsigned long long)grown, grown ? (double)peak / (double)grown : 0.0);
    return 0;
  }
  double min_s = argc > 3 ? atof(argv[3]) : 0.5;
  replay(ptr, sizes, 0, NULL); /* warm-up pass */
  double t0 = now(), t;
  size_t passes = 0;
  do {
    if (replay(ptr, sizes, 0, NULL) != 0) return 1;
    passes++;
    t = now() - t0;
  } while (t < min_s);
  printf("{\"mode\":\"time\",\"ops\":%zu,\"seconds\":%.6f,\"ops_per_sec\":%.1f}\n", nops * passes, t,
         (double)(nops * passes) / t);
  return 0;
}
