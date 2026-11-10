/* SPDX-License-Identifier: Apache-2.0 */
/* The ESP-IDF side of the RTSP server: the listener, a task per control
 * connection and per event channel, sockets, and the platform functions of
 * rtsp_port.h. The request rules are in rtsp_core.c. */
#include "rtsp_server.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "rtsp_core.h"
#include "rtsp_log.h"
#include "rtsp_port.h"
#include "settings.h"
#include "mdns_airplay.h"
#include "wifi.h"
#include "ap2_port.h"

#define RTSP_TCP_PORT 7000
#define MAX_CONTROL 8
/* Control connections and event ports: every session SETUP, holder or not,
 * has its own event port, so each control connection has room for one. */
#define MAX_SLOTS (2 * MAX_CONTROL)
#define LISTEN_STACK 4096      /* internal RAM */
#define CONN_STACK 16384       /* PSRAM; pairing runs here */
#define EVENT_STACK 8192       /* PSRAM */
#define SETTINGS_STACK 3072    /* internal RAM: it may write flash */
#define TASK_PRIORITY 5
#define POLL_MS 100
#define RECV_BUF 2048
#define SEND_TIMEOUT_S 10
#define STOP_WAIT_MS 2000

typedef enum { SLOT_FREE, SLOT_CONN, SLOT_EVENT } slot_kind_t;

typedef struct slot {
  slot_kind_t kind;
  TaskHandle_t task;
  volatile bool stop; /* asked to end */
  volatile bool done; /* finished, waits to be deleted */
  int fd;             /* CONN: the connection; EVENT: the event port */
  /* CONN; EVENT: the connection whose session SETUP opened the port */
  rtsp_conn_t *conn;
  struct slot *event; /* CONN: its event port while it listens */
  /* EVENT */
  int ev_fd;
  uint8_t write_key[32];
  uint8_t read_key[32];
} slot_t;

static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;
static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;
static slot_t s_slots[MAX_SLOTS];
static volatile bool s_running;  /* the listener task is alive */
static volatile bool s_stopping;
static bool s_reset_in_progress;
#ifdef CONFIG_AIRPLAY_HOMEKIT
static char s_reset_name[128];
#endif

static void wipe(void *p, size_t n) {
  volatile uint8_t *v = p;
  while (n--) *v++ = 0;
}

/* ---- Platform functions (rtsp_port.h) ---- */

static void ensure_lock(void) {
  if (s_lock != NULL) return;
  taskENTER_CRITICAL(&s_init_mux);
  if (s_lock == NULL) s_lock = xSemaphoreCreateRecursiveMutexStatic(&s_lock_buf);
  taskEXIT_CRITICAL(&s_init_mux);
}

void rtsp_port_lock(void) {
  ensure_lock();
  xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

void rtsp_port_unlock(void) { xSemaphoreGiveRecursive(s_lock); }

void *rtsp_port_malloc(size_t size) {
  return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void rtsp_port_free(void *ptr) { heap_caps_free(ptr); }

#ifdef CONFIG_AIRPLAY_HOMEKIT
void rtsp_port_homekit_reset(void) { ap2_port_homekit_reset_schedule(); }
esp_err_t rtsp_port_homekit_reset_name(char *name, size_t len) {
  if (!name || !len) return ESP_ERR_INVALID_ARG;
  rtsp_port_lock();
  snprintf(name, len, "%s", s_reset_name);
  rtsp_port_unlock();
  return ESP_OK;
}
#endif

void rtsp_port_pairing_memory(rtsp_pairing_memory_t *out) {
  const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  out->internal_free = heap_caps_get_free_size(caps);
  out->internal_largest = heap_caps_get_largest_free_block(caps);
  out->psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  out->psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
}

uint32_t rtsp_port_now_ms(void) {
  return (uint32_t)(esp_timer_get_time() / 1000);
}

void rtsp_port_device_mac(uint8_t mac[6]) {
  if (ap2_port_device_mac(mac) != ESP_OK) memset(mac, 0, 6);
}

/* Settings may read and write flash. With the cache off a task whose stack
 * is in PSRAM cannot run, so each call runs on a short task with its stack
 * in internal RAM, and the caller waits for it. */
typedef enum {
  SET_GET_VOLUME,
  SET_SET_VOLUME,
  SET_PERSIST_VOLUME,
  SET_GET_NAME,
  SET_SET_NAME,
  SET_ANNOUNCE_NAME
} settings_op_t;

typedef struct {
  settings_op_t op;
  float volume;
  char name[128];
  esp_err_t err;
  TaskHandle_t waiter;
} settings_job_t;

static void settings_task(void *arg) {
  settings_job_t *job = arg;
  switch (job->op) {
    case SET_GET_VOLUME:
      job->err = settings_get_volume(&job->volume);
      break;
    case SET_SET_VOLUME:
      job->err = settings_set_volume(job->volume);
      break;
    case SET_PERSIST_VOLUME:
      job->err = settings_persist_volume();
      break;
    case SET_GET_NAME:
      job->err = settings_get_device_name(job->name, sizeof(job->name));
      job->name[sizeof(job->name) - 1] = '\0';
      break;
    case SET_SET_NAME:
      job->err = settings_set_device_name(job->name);
      if (job->err == ESP_OK)
        job->err = settings_get_device_name(job->name, sizeof(job->name));
      break;
    case SET_ANNOUNCE_NAME:
      /* The saved name, as settings normalized it. */
      job->err = settings_get_device_name(job->name, sizeof(job->name));
      if (job->err == ESP_OK) {
        wifi_set_hostname(job->name);
        job->err = mdns_airplay_update_name(job->name);
      }
      break;
  }
  xTaskNotifyGive(job->waiter);
  vTaskDelete(NULL);
}

static settings_job_t *run_settings(settings_op_t op, float volume, const char *name) {
  settings_job_t *job =
      heap_caps_calloc(1, sizeof(*job), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (job == NULL) return NULL;
  job->op = op;
  job->volume = volume;
  if (name) snprintf(job->name, sizeof(job->name), "%s", name);
  job->err = ESP_FAIL;
  job->waiter = xTaskGetCurrentTaskHandle();
  if (xTaskCreate(settings_task, "rtsp_set", SETTINGS_STACK, job,
                  uxTaskPriorityGet(NULL), NULL) != pdPASS) {
    heap_caps_free(job);
    return NULL;
  }
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  return job;
}

esp_err_t rtsp_port_settings_get_volume(float *volume) {
  settings_job_t *job = run_settings(SET_GET_VOLUME, 0.0f, NULL);
  esp_err_t err;
  if (job == NULL) return ESP_ERR_NO_MEM;
  err = job->err;
  if (err == ESP_OK) *volume = job->volume;
  heap_caps_free(job);
  return err;
}

static esp_err_t run_settings_err(settings_op_t op, float volume) {
  settings_job_t *job = run_settings(op, volume, NULL);
  esp_err_t err;
  if (job == NULL) return ESP_ERR_NO_MEM;
  err = job->err;
  heap_caps_free(job);
  return err;
}

esp_err_t rtsp_port_settings_set_volume(float volume) {
  return run_settings_err(SET_SET_VOLUME, volume);
}

esp_err_t rtsp_port_settings_persist_volume(void) {
  return run_settings_err(SET_PERSIST_VOLUME, 0.0f);
}

esp_err_t rtsp_port_settings_get_name(char *name, size_t len) {
  settings_job_t *job = run_settings(SET_GET_NAME, 0.0f, NULL);
  esp_err_t err;
  if (job == NULL) return ESP_ERR_NO_MEM;
  err = job->err;
  if (err == ESP_OK && len > 0) {
    strncpy(name, job->name, len - 1);
    name[len - 1] = '\0';
#ifdef CONFIG_AIRPLAY_HOMEKIT
    rtsp_port_lock();
    snprintf(s_reset_name, sizeof(s_reset_name), "%s", job->name);
    rtsp_port_unlock();
#endif
  }
  heap_caps_free(job);
  return err;
}

esp_err_t rtsp_port_settings_set_name(const char *name) {
  if (!name || strnlen(name, 65) > 64) return ESP_ERR_INVALID_ARG;
  settings_job_t *job = run_settings(SET_SET_NAME, 0.0f, name);
  if (!job) return ESP_ERR_NO_MEM;
  const esp_err_t err = job->err;
#ifdef CONFIG_AIRPLAY_HOMEKIT
  if (err == ESP_OK) {
    rtsp_port_lock();
    snprintf(s_reset_name, sizeof(s_reset_name), "%s", job->name);
    rtsp_port_unlock();
  }
#endif
  heap_caps_free(job);
  return err;
}

esp_err_t rtsp_port_settings_announce_name(void) {
  return run_settings_err(SET_ANNOUNCE_NAME, 0.0f);
}

esp_err_t rtsp_port_udp_pair(uint16_t *data_port, uint16_t *control_port) {
  int fds[2] = {-1, -1};
  uint16_t ports[2] = {0, 0};
  esp_err_t err = ESP_OK;
  for (int i = 0; i < 2 && err == ESP_OK; i++) {
    struct sockaddr_in addr = {0};
    socklen_t len = sizeof(addr);
    fds[i] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = 0;
    if (fds[i] < 0 || bind(fds[i], (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        getsockname(fds[i], (struct sockaddr *)&addr, &len) != 0) {
      err = ESP_FAIL;
    } else {
      ports[i] = ntohs(addr.sin_port);
    }
  }
  for (int i = 0; i < 2; i++) {
    if (fds[i] >= 0) close(fds[i]);
  }
  if (err != ESP_OK) return err;
  *data_port = ports[0];
  *control_port = ports[1];
  return ESP_OK;
}

/* ---- Slots ---- */

static slot_t *slot_alloc(slot_kind_t kind) {
  slot_t *found = NULL;
  int control = 0;
  rtsp_port_lock();
  for (int i = 0; i < MAX_SLOTS; i++) {
    if (s_slots[i].kind == SLOT_CONN) control++;
  }
  if (kind != SLOT_CONN || control < MAX_CONTROL) {
    for (int i = 0; i < MAX_SLOTS && found == NULL; i++) {
      if (s_slots[i].kind == SLOT_FREE) {
        found = &s_slots[i];
        memset(found, 0, sizeof(*found));
        found->kind = kind;
        found->fd = -1;
        found->ev_fd = -1;
      }
    }
  }
  rtsp_port_unlock();
  return found;
}

static void slot_free(slot_t *slot) {
  rtsp_port_lock();
  wipe(slot, sizeof(*slot));
  slot->kind = SLOT_FREE;
  rtsp_port_unlock();
}

/* Deletes the tasks that finished. A finished task suspends itself; it is
 * deleted only once suspended, so never while it runs on the other core. */
static int reap(void) {
  int used = 0;
  for (int i = 0; i < MAX_SLOTS; i++) {
    slot_t *slot = &s_slots[i];
    if (slot->kind == SLOT_FREE) continue;
    if (slot->done && slot->task != NULL &&
        eTaskGetState(slot->task) == eSuspended) {
      vTaskDeleteWithCaps(slot->task);
      slot_free(slot);
      continue;
    }
    used++;
  }
  return used;
}

static void finish_task(slot_t *slot) {
  slot->done = true;
  vTaskSuspend(NULL);
}

static void set_timeouts(int fd, int recv_ms) {
  struct timeval rcv = {.tv_sec = recv_ms / 1000,
                        .tv_usec = (recv_ms % 1000) * 1000};
  struct timeval snd = {.tv_sec = SEND_TIMEOUT_S, .tv_usec = 0};
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

static int send_all(void *ctx, const uint8_t *data, size_t len) {
  int fd = *(int *)ctx;
  while (len > 0) {
    int n = send(fd, data, len, 0);
    if (n <= 0) return -1;
    data += n;
    len -= (size_t)n;
  }
  return 0;
}

/* ---- Event channel ---- */

static void event_close_channel(slot_t *ev, rtsp_evconn_t **chan) {
  if (*chan != NULL) {
    rtsp_evconn_free(*chan);
    *chan = NULL;
  }
  if (ev->ev_fd >= 0) {
    close(ev->ev_fd);
    ev->ev_fd = -1;
  }
}

static void event_task(void *arg) {
  slot_t *ev = arg;
  rtsp_evconn_t *chan = NULL;
  uint8_t *buf = heap_caps_malloc(RECV_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  while (!ev->stop && buf != NULL) {
    fd_set rfds;
    struct timeval tv = {.tv_sec = 0, .tv_usec = 50 * 1000};
    int maxfd = ev->fd;
    int n;
    FD_ZERO(&rfds);
    FD_SET(ev->fd, &rfds);
    if (ev->ev_fd >= 0) {
      FD_SET(ev->ev_fd, &rfds);
      if (ev->ev_fd > maxfd) maxfd = ev->ev_fd;
    }
    n = select(maxfd + 1, &rfds, NULL, NULL, &tv);
    if (n > 0 && FD_ISSET(ev->fd, &rfds)) {
      int fd = accept(ev->fd, NULL, NULL);
      if (fd >= 0) {
        /* A new event connection replaces the old one. */
        event_close_channel(ev, &chan);
        ev->ev_fd = fd;
        set_timeouts(fd, POLL_MS);
        rtsp_port_lock();
        /* Not after the port was closed: then ev->conn may be gone. */
        if (!ev->stop) chan = rtsp_evconn_new(ev->conn, ev->write_key, ev->read_key);
        rtsp_port_unlock();
        if (chan == NULL) event_close_channel(ev, &chan);
      }
    }
    if (chan != NULL && n > 0 && ev->ev_fd >= 0 && FD_ISSET(ev->ev_fd, &rfds)) {
      int got = recv(ev->ev_fd, buf, RECV_BUF, MSG_DONTWAIT);
      if (got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        event_close_channel(ev, &chan);
      } else if (got > 0 && rtsp_evconn_receive(chan, buf, (size_t)got) != 0) {
        event_close_channel(ev, &chan);
      }
    }
    if (chan != NULL &&
        rtsp_evconn_poll(chan, rtsp_port_now_ms(), send_all, &ev->ev_fd) != 0) {
      event_close_channel(ev, &chan);
    }
  }
  event_close_channel(ev, &chan);
  rtsp_port_lock();
  if (ev->fd >= 0) close(ev->fd);
  ev->fd = -1;
  wipe(ev->write_key, sizeof(ev->write_key));
  wipe(ev->read_key, sizeof(ev->read_key));
  rtsp_port_unlock();
  heap_caps_free(buf);
  finish_task(ev);
}

esp_err_t rtsp_port_event_open(rtsp_conn_t *conn, const uint8_t write_key[32],
                               const uint8_t read_key[32], uint16_t *port) {
  slot_t *slot = rtsp_conn_port_ctx(conn);
  struct sockaddr_in addr = {0};
  socklen_t len = sizeof(addr);
  slot_t *ev;
  int fd;
  if (s_stopping) return ESP_FAIL;
  fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) return ESP_FAIL;
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = 0;
  /* Listening before the SETUP reply goes out (spec "Event channel"). */
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      getsockname(fd, (struct sockaddr *)&addr, &len) != 0 ||
      listen(fd, 1) != 0) {
    close(fd);
    return ESP_FAIL;
  }
  ev = slot_alloc(SLOT_EVENT);
  if (ev == NULL) {
    close(fd);
    return ESP_ERR_NO_MEM;
  }
  ev->fd = fd;
  ev->conn = conn;
  memcpy(ev->write_key, write_key, 32);
  memcpy(ev->read_key, read_key, 32);
  if (xTaskCreateWithCaps(event_task, "rtsp_event", EVENT_STACK, ev,
                          TASK_PRIORITY, &ev->task, MALLOC_CAP_SPIRAM) != pdPASS) {
    close(fd);
    slot_free(ev);
    return ESP_ERR_NO_MEM;
  }
  slot->event = ev;
  *port = ntohs(addr.sin_port);
  return ESP_OK;
}

void rtsp_port_event_close(rtsp_conn_t *conn) {
  slot_t *slot = rtsp_conn_port_ctx(conn);
  rtsp_port_lock();
  if (slot->event != NULL) {
    slot->event->stop = true;
    slot->event->conn = NULL;
    slot->event = NULL;
  }
  rtsp_port_unlock();
}

/* ---- Control connections ---- */

void rtsp_port_conn_close(rtsp_conn_t *conn) {
  slot_t *slot = rtsp_conn_port_ctx(conn);
  rtsp_port_lock();
  slot->stop = true;
  if (slot->fd >= 0) shutdown(slot->fd, SHUT_RDWR);
  rtsp_port_unlock();
}

static void conn_task(void *arg) {
  slot_t *slot = arg;
  uint8_t *buf = heap_caps_malloc(RECV_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  while (!slot->stop && buf != NULL) {
    int n = recv(slot->fd, buf, RECV_BUF, 0);
    if (n > 0) {
      if (rtsp_conn_receive(slot->conn, buf, (size_t)n, send_all, &slot->fd) != 0) {
        break;
      }
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
    break; /* closed by the sender or an error */
  }
  rtsp_conn_end(slot->conn);
  rtsp_port_lock();
  slot->conn = NULL;
  close(slot->fd);
  slot->fd = -1;
  rtsp_port_unlock();
  heap_caps_free(buf);
  finish_task(slot);
}

static void accept_one(int listen_fd) {
  struct sockaddr_in addr = {0};
  socklen_t len = sizeof(addr);
  slot_t *slot;
  int fd = accept(listen_fd, (struct sockaddr *)&addr, &len);
  if (fd < 0) return;
  slot = slot_alloc(SLOT_CONN);
  if (slot == NULL) {
    RTSP_LOGW("too many connections, refused");
    close(fd);
    return;
  }
  set_timeouts(fd, POLL_MS);
  slot->fd = fd;
  slot->conn = rtsp_conn_new(addr.sin_addr.s_addr, slot);
  if (slot->conn == NULL) {
    RTSP_LOGW("no memory for a connection");
    close(fd);
    slot_free(slot);
    return;
  }
  if (xTaskCreateWithCaps(conn_task, "rtsp_conn", CONN_STACK, slot,
                          TASK_PRIORITY, &slot->task, MALLOC_CAP_SPIRAM) != pdPASS) {
    RTSP_LOGW("no memory for a connection task");
    rtsp_conn_end(slot->conn);
    close(fd);
    slot_free(slot);
    return;
  }
  {
    const uint8_t *ip = (const uint8_t *)&addr.sin_addr.s_addr;
    RTSP_LOGI("connection from %u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  }
}

static void stop_all(void) {
  rtsp_port_lock();
  for (int i = 0; i < MAX_SLOTS; i++) {
    slot_t *slot = &s_slots[i];
    if (slot->kind == SLOT_FREE) continue;
    slot->stop = true;
    if (slot->kind == SLOT_CONN && slot->fd >= 0) shutdown(slot->fd, SHUT_RDWR);
  }
  rtsp_port_unlock();
}

static void listener_task(void *arg) {
  int listen_fd = (int)(intptr_t)arg;

  while (!s_stopping) {
    fd_set rfds;
    struct timeval tv = {.tv_sec = 0, .tv_usec = POLL_MS * 1000};
    int n;
    FD_ZERO(&rfds);
    FD_SET(listen_fd, &rfds);
    n = select(listen_fd + 1, &rfds, NULL, NULL, &tv);
    reap();
    if (n > 0 && FD_ISSET(listen_fd, &rfds)) accept_one(listen_fd);
    if (n < 0 && errno != EINTR) vTaskDelay(pdMS_TO_TICKS(POLL_MS));
  }
  close(listen_fd);
  /* Every connection ends; the session ends with its connection. */
  for (;;) {
    stop_all();
    if (reap() == 0) break;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  rtsp_port_lock();
  s_running = false;
  rtsp_port_unlock();
  RTSP_LOGI("stopped");
  vTaskDelete(NULL);
}

/* ---- rtsp_server.h ---- */

void rtsp_server_begin_reset(void) {
  rtsp_port_lock();
  s_reset_in_progress = true;
  rtsp_port_unlock();
}

esp_err_t rtsp_server_start(void) {
  struct sockaddr_in addr = {0};
  int one = 1;
  int fd;

  rtsp_port_lock();
  if (s_reset_in_progress) {
    rtsp_port_unlock();
    return ESP_ERR_INVALID_STATE;
  }
  if (s_running) {
    rtsp_port_unlock();
    /* still ending after a stop (spec "Interface to the firmware") */
    return s_stopping ? ESP_ERR_INVALID_STATE : ESP_OK;
  }
#ifdef CONFIG_AIRPLAY_HOMEKIT
  /* start() runs on an internal-stack startup/maintenance task, before
   * streaming. Removal can answer without creating a settings worker. */
  if (settings_get_device_name(s_reset_name, sizeof(s_reset_name)) != ESP_OK)
    s_reset_name[0] = '\0';
#endif
  fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) {
    rtsp_port_unlock();
    return ESP_FAIL;
  }
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(RTSP_TCP_PORT);
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      listen(fd, 4) != 0) {
    RTSP_LOGE("port %d: bind/listen failed (errno %d)", RTSP_TCP_PORT, errno);
    close(fd);
    rtsp_port_unlock();
    return ESP_FAIL;
  }
  s_stopping = false;
  s_running = true;
  if (xTaskCreate(listener_task, "rtsp_listen", LISTEN_STACK,
                  (void *)(intptr_t)fd, TASK_PRIORITY, NULL) != pdPASS) {
    s_running = false;
    close(fd);
    rtsp_port_unlock();
    return ESP_ERR_NO_MEM;
  }
  rtsp_port_unlock();
  RTSP_LOGI("listening on TCP %d", RTSP_TCP_PORT);
  return ESP_OK;
}

void rtsp_server_stop(void) {
  TickType_t start = xTaskGetTickCount();
  rtsp_port_lock();
  if (s_running && !s_stopping) {
    s_stopping = true;
    RTSP_LOGI("stopping");
  }
  rtsp_port_unlock();
  if (s_running) stop_all();
  /* Waits for the listener to end every task (spec "Interface to the
   * firmware": at most 2 s, seen about 0.1 s). Not under the lock, which
   * the tasks need to end. */
  while (!rtsp_server_is_idle()) {
    if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(STOP_WAIT_MS)) {
      RTSP_LOGW("not idle %d ms after stop", STOP_WAIT_MS);
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

bool rtsp_server_is_idle(void) {
  bool idle;
  rtsp_port_lock();
  idle = !s_running && rtsp_core_idle();
  for (int i = 0; i < MAX_SLOTS && idle; i++) {
    if (s_slots[i].kind != SLOT_FREE) idle = false;
  }
  rtsp_port_unlock();
  return idle;
}
