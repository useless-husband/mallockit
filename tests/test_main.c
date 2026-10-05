#include "test.h"

#include <string.h>
#include <time.h>

#define MAX_TESTS 256
static struct {
  const char *name;
  mk_test_fn fn;
} tests[MAX_TESTS];
static int ntests;
int mk_test_current_failed;
uint64_t mk_test_seed;

void mk_test_register(const char *name, mk_test_fn fn) {
  if (ntests < MAX_TESTS) {
    tests[ntests].name = name;
    tests[ntests].fn = fn;
    ntests++;
  }
}

void mk_test_fail(const char *file, int line, const char *expr) {
  mk_test_current_failed = 1;
  printf("    FAIL %s:%d: %s", file, line, expr);
  if (mk_test_seed) printf("  (seed %" PRIu64 ")", mk_test_seed);
  printf("\n");
  fflush(stdout);
}

int mk_test_scale(void) {
  const char *s = getenv("MK_TEST_SCALE");
  int v = s ? atoi(s) : 1;
  return v > 0 ? v : 1;
}

static int by_name(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int main(int argc, char **argv) {
  /* stable order: constructor order differs between linkers */
  qsort(tests, (size_t)ntests, sizeof(tests[0]), by_name);
  int passed = 0, failed = 0, skipped = 0;
  for (int i = 0; i < ntests; i++) {
    if (argc > 1 && strstr(tests[i].name, argv[1]) == NULL) {
      skipped++;
      continue;
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    mk_test_current_failed = 0;
    mk_test_seed = 0;
    tests[i].fn();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("%s %-40s %8.1f ms\n", mk_test_current_failed ? "FAIL" : "ok  ", tests[i].name, ms);
    fflush(stdout);
    if (mk_test_current_failed) failed++; else passed++;
  }
  printf("%d passed, %d failed", passed, failed);
  if (skipped) printf(", %d not selected", skipped);
  printf("\n");
  return failed ? 1 : 0;
}
