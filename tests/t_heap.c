/* Heap layer: per-thread heaps, abandon on exit, adoption, reclaim, fork. */
#include "test.h"

#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>

static void *heap_of_thread(void *arg) {
  (void)arg;
  void *p = mk_malloc(10);
  void *h = mk_heap_get();
  mk_free(p);
  return h;
}

TEST(heap_one_per_thread) {
  mk_free(mk_malloc(1));
  void *mine = mk_heap_get(), *other = NULL;
  pthread_t t;
  pthread_create(&t, NULL, heap_of_thread, NULL);
  pthread_join(t, &other);
  CHECK(mine != NULL && other != NULL && mine != other);
}

typedef struct {
  void *blocks[64];
  mk_heap_t *heap;
  uintptr_t tid;
} leave_t;

static void *alloc_and_exit(void *arg) {
  leave_t *l = arg;
  for (int i = 0; i < 64; i++) l->blocks[i] = mk_malloc(100);
  l->heap = mk_heap_get();
  return NULL;
}

static void *adopt_and_free(void *arg) {
  leave_t *l = arg;
  void *x = mk_malloc(16); /* attaches a heap: the abandoned one */
  l->tid = mk_thread_id();
  bool adopted = mk_heap_get() == l->heap;
  mk_segment_t *s = mk_segment_of(l->blocks[0]);
  bool owns = atomic_load(&s->thread_id) == mk_thread_id();
  for (int i = 0; i < 64; i++) mk_free(l->blocks[i]); /* local frees now */
  mk_free(x);
  return (void *)(uintptr_t)(adopted && owns);
}

TEST(heap_abandoned_then_adopted) {
  mk_collect(true); /* no other abandoned heap in the list */
  leave_t l = {0};
  pthread_t t;
  pthread_create(&t, NULL, alloc_and_exit, &l);
  pthread_join(t, NULL);
  mk_segment_t *s = mk_segment_of(l.blocks[0]);
  CHECK(atomic_load(&s->thread_id) == 0); /* no owner while abandoned */
  CHECK(l.heap->abandoned);
  void *ok = NULL;
  pthread_create(&t, NULL, adopt_and_free, &l);
  pthread_join(t, &ok);
  CHECK(ok == (void *)1);
}

TEST(heap_reclaim_frees_abandoned_memory) {
  mk_collect(true);
  leave_t l = {0};
  pthread_t t;
  pthread_create(&t, NULL, alloc_and_exit, &l);
  pthread_join(t, NULL);
  for (int i = 0; i < 64; i++) mk_free(l.blocks[i]); /* remote frees into an abandoned heap */
  mk_stats_t a;
  mk_stats_get(&a);
  CHECK(a.segments_in_use >= 1);
  mk_collect(false); /* reclaims abandoned heaps */
  mk_stats_get(&a);
  CHECK(a.segments_in_use == 0);
}

TEST(heap_thread_done_is_explicit_exit) {
  void *p = mk_malloc(10);
  mk_heap_t *h = mk_heap_get();
  mk_thread_done();
  CHECK(mk_heap_get() == NULL);
  CHECK(h->abandoned);
  mk_free(p);         /* remote path now */
  void *q = mk_malloc(10); /* attaches a heap again (likely the same one) */
  CHECK(mk_heap_get() != NULL);
  mk_free(q);
}

static _Atomic(int) fork_go, fork_ready;
static void *hold_blocks(void *arg) {
  void **out = arg;
  for (int i = 0; i < 100; i++) out[i] = mk_malloc(64);
  atomic_store(&fork_ready, 1);
  while (atomic_load(&fork_go) == 0) usleep(1000);
  return NULL;
}

/* fork while another thread owns a heap: the child can still allocate,
 * free its own and the other thread's blocks, and start threads. */
TEST(heap_fork_child_keeps_working) {
  void *theirs[100] = {0};
  pthread_t t;
  pthread_create(&t, NULL, hold_blocks, theirs);
  while (atomic_load(&fork_ready) == 0) usleep(1000);
  void *mine = mk_malloc(1000);
  pid_t pid = fork();
  if (pid == 0) {
    int bad = 0;
    for (int i = 0; i < 100; i++) mk_free(theirs[i]);
    mk_free(mine);
    for (int i = 0; i < 10000; i++) {
      void *x = mk_malloc((size_t)(i % 3000));
      if (!x) bad = 1;
      mk_free(x);
    }
#if !MK_UNDER_TSAN /* TSan cannot start threads after a multi-threaded fork */
    pthread_t c;
    if (pthread_create(&c, NULL, heap_of_thread, NULL) != 0) bad = 1;
    else pthread_join(c, NULL);
#endif
    _exit(bad);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  atomic_store(&fork_go, 1);
  pthread_join(t, NULL);
  for (int i = 0; i < 100; i++) mk_free(theirs[i]);
  mk_free(mine);
  if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0))
    printf("    child status %d signal %d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1, WIFSIGNALED(st) ? WTERMSIG(st) : 0);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
}
