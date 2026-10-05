/* A very small test harness: TEST(name) registers a function, CHECK fails
 * the current test with file:line and, if set, the random seed in use. */
#ifndef MK_TEST_H
#define MK_TEST_H
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../src/internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

typedef void (*mk_test_fn)(void);
void mk_test_register(const char *name, mk_test_fn fn);
void mk_test_fail(const char *file, int line, const char *expr);
extern int mk_test_current_failed;
extern uint64_t mk_test_seed; /* printed on failure when non-zero */
int mk_test_scale(void);      /* MK_TEST_SCALE: divide the work (sanitizer runs) */

#define TEST(name)                                                                                                    \
  static void test_##name(void);                                                                                      \
  __attribute__((constructor)) static void reg_##name(void) { mk_test_register(#name, test_##name); }               \
  static void test_##name(void)

#define CHECK(cond)                                                                                                   \
  do {                                                                                                                \
    if (!(cond)) {                                                                                                    \
      mk_test_fail(__FILE__, __LINE__, #cond);                                                                        \
      return;                                                                                                         \
    }                                                                                                                 \
  } while (0)

/* For helpers that return a value: CHECK_R(cond, value) */
#define CHECK_R(cond, ret)                                                                                            \
  do {                                                                                                                \
    if (!(cond)) {                                                                                                    \
      mk_test_fail(__FILE__, __LINE__, #cond);                                                                        \
      return ret;                                                                                                     \
    }                                                                                                                 \
  } while (0)

/* xorshift64*: deterministic, seedable */
static inline uint64_t mk_rand(uint64_t *s) {
  uint64_t x = *s;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  *s = x;
  return x * 0x2545F4914F6CDD1Dull;
}
#endif
