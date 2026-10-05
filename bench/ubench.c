/* Small in-process micro-benchmarks: mallockit (mk_*) against the
 * malloc this program was linked with, through the same function
 * pointers. Used by the launcher for a quick, self-contained comparison;
 * the real comparison against other allocators is bench/run.py. */
#define _GNU_SOURCE
#include "../include/mallockit.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
  const char *name;
  void *(*alloc)(size_t);
  void (*release)(void *);
} api_t;

static const api_t apis[2] = {{"system", malloc, free}, {"mallockit", mk_malloc, mk_free}};
static int quick;

static double now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static uint64_t rnd(uint64_t *s) {
  *s ^= *s << 13;
  *s ^= *s >> 7;
  *s ^= *s << 17;
  return *s;
}

/* malloc+free of one size, ns per pair */
static double b_pair(const api_t *a, size_t n) {
  long iters = quick ? 2000000 : 10000000;
  void *volatile sink;
  double t0 = now();
  for (long i = 0; i < iters; i++) {
    void *p = a->alloc(n);
    sink = p;
    a->release(p);
  }
  (void)sink;
  return (now() - t0) * 1e9 / (double)iters;
}

/* allocate a batch of random sizes, free it in random order; ns per op */
static double b_batch(const api_t *a) {
  enum { N = 100000 };
  static void *v[N];
  int rounds = quick ? 10 : 50;
  uint64_t s = 88172645463325252ull;
  double t0 = now();
  for (int r = 0; r < rounds; r++) {
    for (int i = 0; i < N; i++) v[i] = a->alloc(16 + rnd(&s) % 512);
    for (int i = N - 1; i > 0; i--) {
      int j = (int)(rnd(&s) % (uint64_t)(i + 1));
      void *t = v[i];
      v[i] = v[j];
      v[j] = t;
    }
    for (int i = 0; i < N; i++) a->release(v[i]);
  }
  return (now() - t0) * 1e9 / (2.0 * N * rounds);
}

/* one thread allocates, another frees (single-producer ring) */
#define RING 1024
typedef struct {
  const api_t *a;
  void *_Atomic slot[RING];
  long n;
} ring_t;

static void *consumer(void *arg) {
  ring_t *r = arg;
  for (long i = 0; i < r->n; i++) {
    void *p;
    while ((p = atomic_exchange_explicit(&r->slot[i % RING], NULL, memory_order_acquire)) == NULL) {
    }
    r->a->release(p);
  }
  return NULL;
}

static double b_xthread(const api_t *a) {
  static ring_t r;
  memset(&r, 0, sizeof r);
  r.a = a;
  r.n = quick ? 1000000 : 5000000;
  pthread_t t;
  double t0 = now();
  pthread_create(&t, NULL, consumer, &r);
  for (long i = 0; i < r.n; i++) {
    void *p = a->alloc(64 + (size_t)(i & 255));
    while (atomic_load_explicit(&r.slot[i % RING], memory_order_acquire) != NULL) {
    }
    atomic_store_explicit(&r.slot[i % RING], p, memory_order_release);
  }
  pthread_join(t, NULL);
  return (now() - t0) * 1e9 / (double)r.n;
}

static int cmpd(const void *x, const void *y) {
  double a = *(const double *)x, b = *(const double *)y;
  return (a > b) - (a < b);
}

static double median(double (*f)(const api_t *, size_t), const api_t *a, size_t arg, int reps) {
  double v[9];
  for (int i = 0; i < reps; i++) v[i] = f(a, arg);
  qsort(v, (size_t)reps, sizeof(double), cmpd);
  return v[reps / 2];
}
static double w_batch(const api_t *a, size_t n) { (void)n; return b_batch(a); }
static double w_xthread(const api_t *a, size_t n) { (void)n; return b_xthread(a); }

int main(int argc, char **argv) {
  quick = argc > 1 && strcmp(argv[1], "--quick") == 0;
  int reps = quick ? 3 : 5;
  printf("%-26s %12s %12s   (median of %d runs, ns per operation; lower is better)\n", "benchmark", apis[0].name,
         apis[1].name, reps);
  size_t sizes[] = {16, 64, 256, 1024, 4096, 32768};
  for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
    char name[64];
    snprintf(name, sizeof name, "malloc+free %zu B", sizes[i]);
    double s = median(b_pair, &apis[0], sizes[i], reps), m = median(b_pair, &apis[1], sizes[i], reps);
    printf("%-26s %12.1f %12.1f\n", name, s, m);
  }
  double s = median(w_batch, &apis[0], 0, reps), m = median(w_batch, &apis[1], 0, reps);
  printf("%-26s %12.1f %12.1f\n", "batch 100k random free", s, m);
  s = median(w_xthread, &apis[0], 0, reps);
  m = median(w_xthread, &apis[1], 0, reps);
  printf("%-26s %12.1f %12.1f\n", "alloc here, free there", s, m);
  return 0;
}
