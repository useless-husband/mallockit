/* Multi-threaded stress with cross-thread frees. Each test ends with a
 * leak check: after everything is freed and mk_collect(true) has run, no
 * segment may still be in use. A lost remote free (for example a missing
 * atomic) leaves a page with a block that is never freed, and fails it. */
#include "test.h"

#include <pthread.h>

static bool all_memory_returned(void) {
  mk_collect(true);
  mk_stats_t s;
  mk_stats_get(&s);
  if (s.segments_in_use != 0 || s.large_in_use != 0)
    printf("    segments still in use: %zu, large: %zu\n", s.segments_in_use, s.large_in_use);
  return s.segments_in_use == 0 && s.large_in_use == 0;
}

/* Every block starts with a header that lets the receiver verify it. */
typedef struct {
  uint64_t size, seq, check;
} hdr_t;

static void *make_block(uint64_t *r, uint64_t seq) {
  size_t n = sizeof(hdr_t) + (size_t)(mk_rand(r) % 3 == 0 ? mk_rand(r) % 20000 : mk_rand(r) % 300);
  if (mk_rand(r) % 500 == 0) n += 200000; /* the odd large object */
  hdr_t *h = mk_malloc(n);
  if (h == NULL) return NULL;
  h->size = n;
  h->seq = seq;
  h->check = n * 0x9E3779B97F4A7C15ull ^ seq;
  uint8_t *body = (uint8_t *)(h + 1);
  size_t bn = n - sizeof(hdr_t);
  if (bn > 0) body[0] = (uint8_t)seq;
  if (bn > 1) body[bn - 1] = (uint8_t)(seq >> 8);
  return h;
}

static bool block_ok(const hdr_t *h) {
  if (h->check != (h->size * 0x9E3779B97F4A7C15ull ^ h->seq)) return false;
  const uint8_t *body = (const uint8_t *)(h + 1);
  size_t bn = h->size - sizeof(hdr_t);
  if (bn > 0 && body[0] != (uint8_t)h->seq) return false;
  if (bn > 1 && body[bn - 1] != (uint8_t)(h->seq >> 8)) return false;
  return mk_usable_size(h) >= h->size;
}

/* ------------------------------------------- producer / consumer */

#define QCAP 4096
typedef struct {
  pthread_mutex_t mu;
  pthread_cond_t not_empty, not_full;
  void *items[QCAP];
  size_t head, count;
  int producers_left;
  _Atomic(long) bad, consumed;
} queue_t;

static queue_t q;
static long pc_per_producer;

static void *producer(void *arg) {
  uint64_t r = 1000 + (uint64_t)(uintptr_t)arg;
  for (long i = 0; i < pc_per_producer; i++) {
    void *b = make_block(&r, (uint64_t)i);
    pthread_mutex_lock(&q.mu);
    while (q.count == QCAP) pthread_cond_wait(&q.not_full, &q.mu);
    q.items[(q.head + q.count) % QCAP] = b;
    q.count++;
    pthread_cond_signal(&q.not_empty);
    pthread_mutex_unlock(&q.mu);
    /* producers also free some of their own blocks locally */
    if (i % 7 == 0) mk_free(make_block(&r, (uint64_t)i));
  }
  pthread_mutex_lock(&q.mu);
  q.producers_left--;
  pthread_cond_broadcast(&q.not_empty);
  pthread_mutex_unlock(&q.mu);
  return NULL;
}

static void *consumer(void *arg) {
  (void)arg;
  for (;;) {
    pthread_mutex_lock(&q.mu);
    while (q.count == 0 && q.producers_left > 0) pthread_cond_wait(&q.not_empty, &q.mu);
    if (q.count == 0) {
      pthread_mutex_unlock(&q.mu);
      return NULL;
    }
    void *b = q.items[q.head];
    q.head = (q.head + 1) % QCAP;
    q.count--;
    pthread_cond_signal(&q.not_full);
    pthread_mutex_unlock(&q.mu);
    if (!block_ok(b)) atomic_fetch_add(&q.bad, 1);
    mk_free(b); /* remote free: allocated by a producer */
    atomic_fetch_add(&q.consumed, 1);
  }
}

TEST(stress_producer_consumer) {
  enum { P = 4, C = 4 };
  pc_per_producer = 60000 / mk_test_scale();
  memset(&q, 0, sizeof(q));
  pthread_mutex_init(&q.mu, NULL);
  pthread_cond_init(&q.not_empty, NULL);
  pthread_cond_init(&q.not_full, NULL);
  q.producers_left = P;
  pthread_t t[P + C];
  for (int i = 0; i < P; i++) pthread_create(&t[i], NULL, producer, (void *)(uintptr_t)i);
  for (int i = 0; i < C; i++) pthread_create(&t[P + i], NULL, consumer, NULL);
  for (int i = 0; i < P + C; i++) pthread_join(t[i], NULL);
  CHECK(atomic_load(&q.bad) == 0);
  CHECK(atomic_load(&q.consumed) == P * pc_per_producer);
  CHECK(all_memory_returned());
}

/* ----------------------------------------------- larson-like slots */

#define NSLOTS 20000
static _Atomic(hdr_t *) slots[NSLOTS];
static _Atomic(long) slot_bad;
static long slot_ops;

static void *slot_worker(void *arg) {
  uint64_t r = 77 + (uint64_t)(uintptr_t)arg * 1315423911u;
  for (long i = 0; i < slot_ops; i++) {
    hdr_t *nb = make_block(&r, (uint64_t)i);
    hdr_t *old = atomic_exchange(&slots[mk_rand(&r) % NSLOTS], nb);
    if (old != NULL) {
      if (!block_ok(old)) atomic_fetch_add(&slot_bad, 1);
      mk_free(old); /* usually allocated by another thread */
    }
  }
  return NULL;
}

TEST(stress_slot_exchange_cross_thread_frees) {
  enum { T = 8 };
  slot_ops = 150000 / mk_test_scale();
  pthread_t t[T];
  for (int i = 0; i < T; i++) pthread_create(&t[i], NULL, slot_worker, (void *)(uintptr_t)i);
  for (int i = 0; i < T; i++) pthread_join(t[i], NULL);
  long bad = atomic_load(&slot_bad);
  for (int i = 0; i < NSLOTS; i++) {
    hdr_t *b = atomic_exchange(&slots[i], NULL);
    if (b) {
      if (!block_ok(b)) bad++;
      mk_free(b);
    }
  }
  CHECK(bad == 0);
  mk_stats_t s;
  mk_stats_get(&s);
  CHECK(s.remote_frees > 0);
  CHECK(all_memory_returned());
}

/* ------------------------------------------------- thread churn */

/* Rounds of short-lived threads. Each thread frees the blocks a thread of
 * the previous round left behind (owned by an exited, abandoned heap) and
 * leaves its own blocks for the next round. Exercises abandon, adopt,
 * delayed frees into abandoned heaps and reclaiming them. */
#define CHURN_PER_THREAD 3000
#define CHURN_THREADS 6
static void *churn_blocks[CHURN_THREADS][CHURN_PER_THREAD];
static _Atomic(long) churn_bad;

static void *churn_worker(void *arg) {
  uintptr_t id = (uintptr_t)arg;
  uint64_t r = 5 + id * 977;
  void *mine[CHURN_PER_THREAD];
  for (int i = 0; i < CHURN_PER_THREAD; i++) mine[i] = make_block(&r, (uint64_t)i);
  /* hand over: swap our new blocks into our slot, free what was there */
  for (int i = 0; i < CHURN_PER_THREAD; i++) {
    void *old = churn_blocks[id][i];
    churn_blocks[id][i] = mine[i];
    if (old) {
      if (!block_ok(old)) atomic_fetch_add(&churn_bad, 1);
      mk_free(old);
    }
  }
  return NULL;
}

TEST(stress_thread_churn_abandon_adopt) {
  int rounds = 40 / mk_test_scale() + 2;
  mk_stats_t a, b;
  mk_stats_get(&a);
  for (int round = 0; round < rounds; round++) {
    pthread_t t[CHURN_THREADS];
    for (int i = 0; i < CHURN_THREADS; i++) pthread_create(&t[i], NULL, churn_worker, (void *)(uintptr_t)i);
    for (int i = 0; i < CHURN_THREADS; i++) pthread_join(t[i], NULL);
  }
  mk_stats_get(&b);
  /* threads exit and their heaps are adopted instead of piling up */
  CHECK(b.heaps_abandoned - a.heaps_abandoned >= (uint64_t)(rounds * CHURN_THREADS));
  CHECK(b.heaps_adopted > a.heaps_adopted);
  CHECK(b.heaps_created - a.heaps_created <= 2 * CHURN_THREADS);
  for (int i = 0; i < CHURN_THREADS; i++)
    for (int j = 0; j < CHURN_PER_THREAD; j++) {
      if (churn_blocks[i][j] && !block_ok(churn_blocks[i][j])) churn_bad++;
      mk_free(churn_blocks[i][j]); /* blocks of abandoned heaps, freed remotely */
      churn_blocks[i][j] = NULL;
    }
  CHECK(atomic_load(&churn_bad) == 0);
  CHECK(all_memory_returned());
}

/* ------------------------------------ full pages and delayed frees */

/* One thread fills many pages of one size class (they go to the owner's
 * full list), another frees every block. The owner must find all pages
 * again through the delayed-free list. */
static void *free_all(void *arg) {
  void **v = arg;
  for (int i = 0; v[i] != NULL; i++) mk_free(v[i]);
  return NULL;
}

TEST(stress_remote_frees_into_full_pages) {
  enum { N = 40000 };
  void **v = calloc(N + 1, sizeof(void *));
  for (int round = 0; round < 3; round++) {
    for (int i = 0; i < N; i++) v[i] = mk_malloc(48);
    mk_stats_t a, b;
    mk_stats_get(&a);
    pthread_t t;
    pthread_create(&t, NULL, free_all, v);
    pthread_join(t, NULL);
    /* the owner notices on its next slow path */
    for (int i = 0; i < N; i++) v[i] = mk_malloc(48);
    mk_stats_get(&b);
    CHECK(b.delayed_frees > a.delayed_frees);
    CHECK(b.pages_allocated - a.pages_allocated < 20); /* pages were reused, not replaced */
    for (int i = 0; i < N; i++) mk_free(v[i]);
  }
  free(v);
  CHECK(all_memory_returned());
}
