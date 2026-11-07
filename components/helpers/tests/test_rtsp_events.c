/*
 * SPDX-FileCopyrightText: 2026 Kristian8606
 * SPDX-License-Identifier: Apache-2.0
 */

/* Listener rules of rtsp_events. The list cannot be emptied (there is no
 * unregister), so each case runs in its own child process. */

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "rtsp_events.h"
#include "test.h"

#define MAX_CALLS 64

static int calls[MAX_CALLS];
static rtsp_event_t call_events[MAX_CALLS];
static int ncalls;

/* Records which listener (its user_data, an int) got which event. */
static void record(rtsp_event_t event, void *user_data) {
  if (ncalls < MAX_CALLS) {
    calls[ncalls] = *(int *)user_data;
    call_events[ncalls] = event;
  }
  ncalls++;
}

static void record2(rtsp_event_t event, void *user_data) {
  record(event, user_data);
}

static void case_no_listener(void) {
  rtsp_events_emit(RTSP_EVENT_PLAYING);
  CHECK(ncalls == 0);
}

static void case_null(void) {
  int a = 1;
  CHECK(rtsp_events_register(NULL, &a) == -1);
  CHECK(rtsp_events_register(NULL, NULL) == -1);
  rtsp_events_emit(RTSP_EVENT_PLAYING);
  CHECK(ncalls == 0);
}

static void case_order(void) {
  int ids[4] = {10, 20, 30, 40};
  CHECK(rtsp_events_register(record, &ids[0]) == 0);
  CHECK(rtsp_events_register(record2, &ids[1]) == 0);
  CHECK(rtsp_events_register(record, &ids[2]) == 0);
  rtsp_events_emit(RTSP_EVENT_CLIENT_CONNECTED);
  CHECK(ncalls == 3);
  CHECK(calls[0] == 10 && calls[1] == 20 && calls[2] == 30);
  for (int i = 0; i < 3; i++) {
    CHECK(call_events[i] == RTSP_EVENT_CLIENT_CONNECTED);
  }

  CHECK(rtsp_events_register(record2, &ids[3]) == 0);
  ncalls = 0;
  rtsp_events_emit(RTSP_EVENT_DISCONNECTED);
  CHECK(ncalls == 4);
  CHECK(calls[3] == 40 && call_events[3] == RTSP_EVENT_DISCONNECTED);
}

static void case_duplicates(void) {
  int a = 1, b = 2;
  CHECK(rtsp_events_register(record, &a) == 0);
  CHECK(rtsp_events_register(record, &a) == 0);  /* kept once */
  CHECK(rtsp_events_register(record, &b) == 0);  /* other user_data */
  CHECK(rtsp_events_register(record2, &a) == 0); /* other callback */
  CHECK(rtsp_events_register(record, &a) == 0);
  rtsp_events_emit(RTSP_EVENT_PAUSED);
  CHECK(ncalls == 3);
  CHECK(calls[0] == 1 && calls[1] == 2 && calls[2] == 1);
}

static void case_limit(void) {
  int ids[10];
  for (int i = 0; i < 10; i++) {
    ids[i] = i;
  }
  for (int i = 0; i < 8; i++) {
    CHECK(rtsp_events_register(record, &ids[i]) == 0);
  }
  CHECK(rtsp_events_register(record, &ids[8]) == -1);
  CHECK(rtsp_events_register(record2, &ids[9]) == -1);
  /* A duplicate of a registered one is still 0 when full. */
  CHECK(rtsp_events_register(record, &ids[3]) == 0);
  rtsp_events_emit(RTSP_EVENT_PLAYING);
  CHECK(ncalls == 8);
  for (int i = 0; i < 8; i++) {
    CHECK(calls[i] == i);
  }
}

/* A callback that emits again (one level) and registers another listener. */
static int reentrant_depth;
static int late_id = 99;
static void reentrant(rtsp_event_t event, void *user_data) {
  record(event, user_data);
  if (reentrant_depth == 0) {
    reentrant_depth++;
    CHECK(rtsp_events_register(record2, &late_id) == 0);
    rtsp_events_emit(RTSP_EVENT_PAUSED);
    reentrant_depth--;
  }
}

static void case_reentrant(void) {
  int a = 7;
  CHECK(rtsp_events_register(reentrant, &a) == 0);
  rtsp_events_emit(RTSP_EVENT_PLAYING);
  /* Outer call to reentrant, then the inner emit reaches both listeners.
   * Whether the outer emit also reaches the late listener is not fixed. */
  CHECK(ncalls == 3 || ncalls == 4);
  CHECK(calls[0] == 7 && call_events[0] == RTSP_EVENT_PLAYING);
  CHECK(calls[1] == 7 && call_events[1] == RTSP_EVENT_PAUSED);
  CHECK(calls[2] == 99 && call_events[2] == RTSP_EVENT_PAUSED);
}

/* Threads: several emitters and registrations at the same time. */
#define THREADS 8
#define EMITS 20000

static atomic_long counts[THREADS];
static atomic_long late_count;

static void count_cb(rtsp_event_t event, void *user_data) {
  (void)event;
  atomic_fetch_add((atomic_long *)user_data, 1);
}

static void *emitter(void *arg) {
  (void)arg;
  for (int i = 0; i < EMITS; i++) {
    rtsp_events_emit((rtsp_event_t)(i % 4));
  }
  return NULL;
}

static void *registrar(void *arg) {
  (void)arg;
  for (int i = 0; i < 1000; i++) {
    CHECK(rtsp_events_register(count_cb, &late_count) == 0);
  }
  return NULL;
}

static void case_threads(void) {
  /* Four listeners before the threads start, one added while they run. */
  for (int i = 0; i < 4; i++) {
    CHECK(rtsp_events_register(count_cb, &counts[i]) == 0);
  }
  pthread_t t[THREADS + 2];
  for (int i = 0; i < THREADS; i++) {
    pthread_create(&t[i], NULL, emitter, NULL);
  }
  pthread_create(&t[THREADS], NULL, registrar, NULL);
  pthread_create(&t[THREADS + 1], NULL, registrar, NULL);
  for (int i = 0; i < THREADS + 2; i++) {
    pthread_join(t[i], NULL);
  }
  for (int i = 0; i < 4; i++) {
    CHECK(atomic_load(&counts[i]) == (long)THREADS * EMITS);
  }
  CHECK(atomic_load(&late_count) <= (long)THREADS * EMITS);

  /* The late listener was kept once. */
  atomic_store(&late_count, 0);
  rtsp_events_emit(RTSP_EVENT_PLAYING);
  CHECK(atomic_load(&late_count) == 1);
}

static int run(const char *name, void (*fn)(void)) {
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    fn();
    _exit(test_failures == 0 ? 0 : 1);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
  if (!ok) {
    printf("FAIL %s\n", name);
    test_failures++;
  }
  return ok;
}

int main(void) {
  run("no_listener", case_no_listener);
  run("null", case_null);
  run("order", case_order);
  run("duplicates", case_duplicates);
  run("limit", case_limit);
  run("reentrant", case_reentrant);
  run("threads", case_threads);
  return TEST_RESULT();
}
