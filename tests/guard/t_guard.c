/* Guard mode: each test makes one deliberate memory error and checks that
 * the debug build reports exactly that error. */
#include "../test.h"

#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

static _Atomic(int) last_err, nerr;
static const void *_Atomic last_ptr;

static void record(int err, const void *p, const char *msg) {
  (void)msg;
  atomic_store(&last_err, err);
  atomic_store(&last_ptr, p);
  atomic_fetch_add(&nerr, 1);
}

static void reset(void) {
  atomic_store(&last_err, 0);
  atomic_store(&nerr, 0);
  atomic_store(&last_ptr, NULL);
  mk_set_error_handler(record);
}

TEST(guard_is_debug_build) { CHECK(mk_is_debug_build()); }

TEST(guard_clean_use_reports_nothing) {
  reset();
  uint64_t r = 3;
  void *v[512] = {0};
  for (int i = 0; i < 200000; i++) {
    int k = (int)(mk_rand(&r) % 512);
    if (v[k]) {
      mk_free(v[k]);
      v[k] = NULL;
    } else {
      size_t n = mk_rand(&r) % 3000;
      v[k] = (i % 5 == 0) ? mk_realloc(mk_malloc(10), n) : mk_malloc(n);
      memset(v[k], 0x11, n); /* exactly the requested size */
    }
  }
  for (int k = 0; k < 512; k++) mk_free(v[k]);
  CHECK(atomic_load(&nerr) == 0);
}

TEST(guard_usable_size_is_the_requested_size) {
  for (size_t n = 0; n < 300; n++) {
    void *p = mk_malloc(n);
    CHECK(mk_usable_size(p) == n);
    mk_free(p);
  }
}

TEST(guard_detects_overrun_by_one_byte) {
  size_t sizes[] = {1, 13, 16, 24, 100, 1000, 8000, 30000, 100000, 3000000};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    reset();
    uint8_t *p = mk_malloc(sizes[i]);
    p[sizes[i]] = 0; /* one past the end */
    mk_free(p);
    CHECK(atomic_load(&last_err) == MK_ERR_OVERRUN);
    CHECK(atomic_load(&last_ptr) == p);
  }
}

TEST(guard_detects_overrun_of_aligned_block) {
  reset();
  void *q = NULL;
  CHECK(mk_posix_memalign(&q, 256, 100) == 0);
  CHECK(((uintptr_t)q & 255) == 0);
  memset(q, 1, 100);
  mk_free(q);
  CHECK(atomic_load(&nerr) == 0);
  CHECK(mk_posix_memalign(&q, 256, 100) == 0);
  memset(q, 1, 101);
  mk_free(q);
  CHECK(atomic_load(&last_err) == MK_ERR_OVERRUN);
}

TEST(guard_detects_overrun_through_realloc) {
  reset();
  char *p = mk_malloc(10);
  p[10] = 'x';
  p = mk_realloc(p, 20); /* the old block is checked when realloc frees it */
  CHECK(atomic_load(&last_err) == MK_ERR_OVERRUN);
  mk_free(p);
}

TEST(guard_detects_double_free) {
  reset();
  void *keep = mk_malloc(40); /* keeps the page alive */
  void *p = mk_malloc(40);
  mk_free(p);
  CHECK(atomic_load(&nerr) == 0);
  mk_free(p);
  CHECK(atomic_load(&last_err) == MK_ERR_DOUBLE_FREE);
  CHECK(atomic_load(&last_ptr) == p);
  mk_free(keep);
}

static void *free_it(void *p) {
  mk_free(p);
  return NULL;
}

TEST(guard_detects_cross_thread_double_free) {
  reset();
  void *keep = mk_malloc(72);
  void *p = mk_malloc(72);
  pthread_t t;
  pthread_create(&t, NULL, free_it, p);
  pthread_join(t, NULL);
  CHECK(atomic_load(&nerr) == 0);
  pthread_create(&t, NULL, free_it, p);
  pthread_join(t, NULL);
  CHECK(atomic_load(&last_err) == MK_ERR_DOUBLE_FREE);
  mk_free(keep);
}

TEST(guard_detects_invalid_free) {
  reset();
  char *p = mk_malloc(64);
  mk_free(p + 16); /* interior pointer */
  CHECK(atomic_load(&last_err) == MK_ERR_INVALID_FREE);
  reset();
  int on_stack = 0;
  mk_free(&on_stack); /* not ours at all */
  CHECK(atomic_load(&last_err) == MK_ERR_INVALID_FREE);
  reset();
  char *big = mk_malloc(200000);
  mk_free(big + 4096);
  CHECK(atomic_load(&last_err) == MK_ERR_INVALID_FREE);
  mk_free(big);
  mk_free(p);
}

TEST(guard_detects_write_after_free) {
  reset();
  enum { N = 20000 };
  static void *v[N];
  uint8_t *p = mk_malloc(48);
  mk_free(p);
  p[20] = 1; /* use after free */
  int n = 0;
  while (n < N && atomic_load(&nerr) == 0) v[n++] = mk_malloc(48); /* until the block is reused */
  CHECK(atomic_load(&last_err) == MK_ERR_WRITE_AFTER_FREE);
  CHECK(atomic_load(&last_ptr) == p);
  for (int i = 0; i < n; i++) mk_free(v[i]);
}

/* The free-list link of a freed block is overwritten: the allocator must
 * notice when it pops the block, before following the bad link. Runs in
 * a child process because the list is unusable afterwards. */
TEST(guard_detects_corrupted_free_list) {
  pid_t pid = fork();
  if (pid == 0) {
    reset();
    void *keep = mk_malloc(32);
    void **p = mk_malloc(32);
    mk_free(p);
    *p = (void *)(uintptr_t)0x1000; /* dangling write into the link */
    for (int i = 0; i < 100000 && atomic_load(&nerr) == 0; i++) (void)mk_malloc(32);
    (void)keep;
    _exit(atomic_load(&last_err) == MK_ERR_CORRUPT_FREELIST ? 0 : 1);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

/* Without a handler the process prints the error and aborts. */
TEST(guard_default_handler_aborts_with_message) {
  int fds[2];
  CHECK(pipe(fds) == 0);
  pid_t pid = fork();
  if (pid == 0) {
    dup2(fds[1], 2);
    mk_set_error_handler(NULL);
    void *keep = mk_malloc(24);
    void *p = mk_malloc(24);
    mk_free(p);
    mk_free(p);
    (void)keep;
    _exit(0);
  }
  close(fds[1]);
  char buf[256] = {0};
  ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
  close(fds[0]);
  int st = 0;
  waitpid(pid, &st, 0);
  CHECK(n > 0 && strstr(buf, "mallockit: double free at 0x") != NULL);
  CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT);
}
