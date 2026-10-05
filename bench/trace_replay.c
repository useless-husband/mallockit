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
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

typedef struct {
  char op;
  uint32_t id;
  uint64_t size;
} op_t;

static op_t *ops;
static size_t nops, nids;

static void load(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) {
    perror(path);
    exit(2);
  }
  size_t cap = 1 << 20;
  ops = malloc(cap * sizeof(op_t));
  char line[256];
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
    if (nops == cap) {
      cap *= 2;
      ops = realloc(ops, cap * sizeof(op_t));
    }
    ops[nops++] = o;
  }
  fclose(f);
}

static double now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static uint64_t maxrss_bytes(void) {
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
  return (uint64_t)ru.ru_maxrss; /* bytes */
#else
  return (uint64_t)ru.ru_maxrss * 1024; /* KiB */
#endif
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
    uint64_t base = maxrss_bytes(), peak = 0;
    if (replay(ptr, sizes, 1, &peak) != 0) return 1;
    uint64_t grown = maxrss_bytes() - base;
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
