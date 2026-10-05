/* An ordinary C program: it only uses the C library. Run it with the
 * shared library preloaded (LD_PRELOAD / DYLD_INSERT_LIBRARIES); with
 * MALLOCKIT_EXPECT=1 it also checks that memory really comes from
 * mallockit, including memory the C library allocates internally. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <malloc/malloc.h>
#endif

static int failures;
#define EXPECT(c)                                                                                                     \
  do {                                                                                                                \
    if (!(c)) {                                                                                                       \
      fprintf(stderr, "smoke: FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                             \
      failures++;                                                                                                     \
    }                                                                                                                 \
  } while (0)

static void *ring[4096];
static void *worker(void *arg) {
  uintptr_t id = (uintptr_t)arg;
  for (int i = 0; i < 200000; i++) {
    size_t k = (id * 7919 + (size_t)i * 104729) % 4096;
    void *nb = malloc(16 + (size_t)i % 500);
    void *old = __atomic_exchange_n(&ring[k], nb, __ATOMIC_ACQ_REL);
    free(old); /* often another thread's block */
  }
  return NULL;
}

static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int main(void) {
  bool expect = getenv("MALLOCKIT_EXPECT") != NULL;
  bool (*owns)(const void *) = (bool (*)(const void *))dlsym(RTLD_DEFAULT, "mk_owns");
  if (expect) EXPECT(owns != NULL);

  /* plain API */
  char *s = malloc(100);
  strcpy(s, "hello");
  s = realloc(s, 100000);
  EXPECT(strcmp(s, "hello") == 0);
  int *z = calloc(1000, sizeof(int));
  for (int i = 0; i < 1000; i++) EXPECT(z[i] == 0);
  void *al = NULL;
  EXPECT(posix_memalign(&al, 4096, 333) == 0 && ((uintptr_t)al & 4095) == 0);
  void *al2 = aligned_alloc(64, 640);
  EXPECT(al2 && ((uintptr_t)al2 & 63) == 0);
  char *dup = strdup("allocated inside the C library");
  if (expect && owns) {
    EXPECT(owns(s) && owns(z) && owns(al) && owns(al2));
    EXPECT(owns(dup)); /* libc's internal malloc call was redirected too */
  }
#if defined(__APPLE__)
  if (expect) {
    /* our zone is registered first, i.e. it is the default zone (libmalloc
     * reports the default zone through a wrapper "DefaultMallocZone") */
    vm_address_t *zones = NULL;
    unsigned nz = 0;
    EXPECT(malloc_get_all_zones(0, NULL, &zones, &nz) == 0 && nz > 0);
    malloc_zone_t *first = nz > 0 ? (malloc_zone_t *)zones[0] : NULL;
    EXPECT(first && first->zone_name && strcmp(first->zone_name, "mallockit") == 0);
    EXPECT(first && first->size(first, s) >= 100000);
    malloc_zone_t *zs = malloc_zone_from_ptr(s); /* ours, or the default-zone wrapper */
    EXPECT(zs == first || zs == malloc_default_zone());
    void *zp = malloc_zone_malloc(malloc_default_zone(), 40); /* the zone path */
    EXPECT(owns && owns(zp));
    EXPECT(malloc_size(zp) >= 40);
    free(zp);
  }
#endif
  free(s);
  free(z);
  free(al);
  free(al2);
  free(dup);

  /* lots of strings, sorted with qsort, freed in a different order */
  enum { N = 50000 };
  char **v = malloc(N * sizeof(char *));
  for (int i = 0; i < N; i++) {
    char buf[32];
    snprintf(buf, sizeof buf, "k%07d", (i * 7919) % N);
    v[i] = strdup(buf);
  }
  qsort(v, N, sizeof(char *), cmp);
  EXPECT(strcmp(v[0], "k0000000") == 0 && strcmp(v[N - 1], "k0049999") == 0);
  for (int i = 0; i < N; i++) free(v[i]);
  free(v);

  /* threads with cross-thread frees */
  pthread_t t[4];
  for (uintptr_t i = 0; i < 4; i++) pthread_create(&t[i], NULL, worker, (void *)i);
  for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
  for (int i = 0; i < 4096; i++) free(ring[i]);

  /* fork: the child allocates and exits cleanly */
  pid_t pid = fork();
  if (pid == 0) {
    char *c = malloc(1 << 20);
    memset(c, 1, 1 << 20);
    free(c);
    _exit(0);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  EXPECT(WIFEXITED(st) && WEXITSTATUS(st) == 0);

  printf("smoke: %s (%s)\n", failures ? "FAILED" : "ok", owns ? "mallockit loaded" : "system allocator");
  return failures ? 1 : 0;
}
