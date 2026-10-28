#include "pcm_fifo.h"

#include <stdlib.h>
#include <string.h>

#ifdef PCM_FIFO_HOST_TEST
/* Host unit test build: no FreeRTOS. */
typedef int pcm_fifo_mutex_t;
#define FIFO_MUTEX_CREATE(m) ((m) = 1, true)
#define FIFO_MUTEX_DELETE(m) ((void)(m))
#define FIFO_LOCK(m) ((void)(m))
#define FIFO_UNLOCK(m) ((void)(m))
#define FIFO_PCM_ALLOC(bytes) malloc(bytes)
#define FIFO_CRIT_ENTER(f) ((void)(f))
#define FIFO_CRIT_EXIT(f) ((void)(f))
#define FIFO_SPIN_US(us) ((void)(us))
#else
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
typedef SemaphoreHandle_t pcm_fifo_mutex_t;
#define FIFO_MUTEX_CREATE(m) (((m) = xSemaphoreCreateMutex()) != NULL)
#define FIFO_MUTEX_DELETE(m) vSemaphoreDelete(m)
#define FIFO_LOCK(m) xSemaphoreTake((m), portMAX_DELAY)
#define FIFO_UNLOCK(m) xSemaphoreGive(m)
#define FIFO_PCM_ALLOC(bytes) \
  heap_caps_malloc((bytes), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
/* An in-place change (odd seq) must never be preempted half-way, or the
 * reader on the other core would see "changing" for milliseconds. */
#define FIFO_CRIT_ENTER(f) taskENTER_CRITICAL(&(f)->mux)
#define FIFO_CRIT_EXIT(f) taskEXIT_CRITICAL(&(f)->mux)
#define FIFO_SPIN_US(us) esp_rom_delay_us(us)
#endif

#define CAP        PCM_FIFO_FRAMES
#define CAP_MASK   (PCM_FIFO_FRAMES - 1U)
#define FRAME_SZ   (PCM_FIFO_CHANNELS * sizeof(pcm_fifo_sample_t))
#define MAX_WRITE  4096U
/* Written frames are never changed. Only the rare window resets (new
 * position, FLUSH, truncate) bump seq, each in a critical section of a few
 * stores; a reader that meets one waits a little and tries again. */
#define READ_TRIES   16
#define READ_WAIT_US 2U

_Static_assert((PCM_FIFO_FRAMES & (PCM_FIFO_FRAMES - 1U)) == 0,
               "PCM_FIFO_FRAMES must be a power of two");

struct pcm_fifo {
  pcm_fifo_sample_t *pcm;
  pcm_fifo_mutex_t mutex; /* writers and control operations */
  uint32_t generation;    /* requested generation (set_generation) */
  uint32_t data_gen;      /* generation the window belongs to */
  uint32_t active;        /* window holds a position (may be empty) */
  uint32_t head;          /* RTP of the oldest frame */
  uint32_t tail;          /* RTP one past the newest frame */
  uint32_t seq;           /* odd while frames inside the window change */
#if defined(CONFIG_AIRPLAY_DIAG_PLAYOUT) && CONFIG_AIRPLAY_DIAG_PLAYOUT
  uint32_t gap_events;    /* silence inserted by writes; writer mutex held */
  uint32_t gap_frames;
#endif
#ifndef PCM_FIFO_HOST_TEST
  portMUX_TYPE mux;       /* makes each odd-seq section non-preemptible */
#endif
};

static inline int32_t rtp_delta(uint32_t a, uint32_t b) {
  return (int32_t)(a - b);
}

static inline uint32_t ld(const uint32_t *p) {
  return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static inline void st(uint32_t *p, uint32_t v) {
  __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

/* Physical copy helpers: at most two pieces around the wrap. */
static void copy_in(pcm_fifo_t *f, uint32_t rtp, const pcm_fifo_sample_t *src,
                    uint32_t n) {
  const uint32_t idx = rtp & CAP_MASK;
  const uint32_t first = n < CAP - idx ? n : CAP - idx;
  memcpy(f->pcm + (size_t)idx * PCM_FIFO_CHANNELS, src, (size_t)first * FRAME_SZ);
  if (n > first) {
    memcpy(f->pcm, src + (size_t)first * PCM_FIFO_CHANNELS,
           (size_t)(n - first) * FRAME_SZ);
  }
}

static void zero_in(pcm_fifo_t *f, uint32_t rtp, uint32_t n) {
  if (n >= CAP) {
    memset(f->pcm, 0, (size_t)CAP * FRAME_SZ);
    return;
  }
  const uint32_t idx = rtp & CAP_MASK;
  const uint32_t first = n < CAP - idx ? n : CAP - idx;
  memset(f->pcm + (size_t)idx * PCM_FIFO_CHANNELS, 0, (size_t)first * FRAME_SZ);
  if (n > first) memset(f->pcm, 0, (size_t)(n - first) * FRAME_SZ);
}

static void copy_out(const pcm_fifo_t *f, uint32_t rtp, pcm_fifo_sample_t *dst,
                     uint32_t n) {
  const uint32_t idx = rtp & CAP_MASK;
  const uint32_t first = n < CAP - idx ? n : CAP - idx;
  memcpy(dst, f->pcm + (size_t)idx * PCM_FIFO_CHANNELS, (size_t)first * FRAME_SZ);
  if (n > first) {
    memcpy(dst + (size_t)first * PCM_FIFO_CHANNELS, f->pcm,
           (size_t)(n - first) * FRAME_SZ);
  }
}

/* Mark the start/end of a change the reader must not mix with. Always
 * inside FIFO_CRIT_ENTER/EXIT, so the odd phase lasts microseconds. */
static inline void seq_begin(pcm_fifo_t *f) {
  st(&f->seq, ld(&f->seq) + 1U);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static inline void seq_end(pcm_fifo_t *f) {
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  st(&f->seq, ld(&f->seq) + 1U);
}

/* Window bounds change (writer lock held). */
static void set_window(pcm_fifo_t *f, uint32_t head, uint32_t tail) {
  FIFO_CRIT_ENTER(f);
  seq_begin(f);
  st(&f->head, head);
  st(&f->tail, tail);
  seq_end(f);
  FIFO_CRIT_EXIT(f);
}

/* Empty window positioned at rtp (writer lock held). */
static void reset_window(pcm_fifo_t *f, uint32_t rtp, uint32_t gen) {
  FIFO_CRIT_ENTER(f);
  seq_begin(f);
  st(&f->head, rtp);
  st(&f->tail, rtp);
  st(&f->data_gen, gen);
  st(&f->active, 1U);
  seq_end(f);
  FIFO_CRIT_EXIT(f);
}

static bool window_current(const pcm_fifo_t *f, uint32_t generation) {
  return ld(&f->active) && ld(&f->data_gen) == generation &&
         ld(&f->generation) == generation;
}

esp_err_t pcm_fifo_create(pcm_fifo_t **out) {
  if (!out) return ESP_ERR_INVALID_ARG;
  *out = NULL;
  pcm_fifo_t *f = calloc(1, sizeof(*f));
  if (!f) return ESP_ERR_NO_MEM;
#ifndef PCM_FIFO_HOST_TEST
  portMUX_INITIALIZE(&f->mux);
#endif
  f->pcm = FIFO_PCM_ALLOC((size_t)CAP * FRAME_SZ);
  if (!f->pcm || !FIFO_MUTEX_CREATE(f->mutex)) {
    free(f->pcm);
    free(f);
    return ESP_ERR_NO_MEM;
  }
  *out = f;
  return ESP_OK;
}

void pcm_fifo_destroy(pcm_fifo_t *f) {
  if (!f) return;
  FIFO_MUTEX_DELETE(f->mutex);
  free(f->pcm);
  free(f);
}

void pcm_fifo_set_generation(pcm_fifo_t *f, uint32_t generation) {
  if (f) st(&f->generation, generation);
}

#if defined(CONFIG_AIRPLAY_DIAG_PLAYOUT) && CONFIG_AIRPLAY_DIAG_PLAYOUT
void pcm_fifo_diag_take(pcm_fifo_t *f, uint32_t *gap_events,
                        uint32_t *gap_frames) {
  if (gap_events) *gap_events = 0U;
  if (gap_frames) *gap_frames = 0U;
  if (!f) return;
  FIFO_LOCK(f->mutex);
  if (gap_events) *gap_events = f->gap_events;
  if (gap_frames) *gap_frames = f->gap_frames;
  f->gap_events = 0U;
  f->gap_frames = 0U;
  FIFO_UNLOCK(f->mutex);
}
#endif

static bool write_locked(pcm_fifo_t *f, uint32_t rtp, const pcm_fifo_sample_t *pcm,
                         uint32_t frames, uint32_t generation,
                         uint32_t wanted, bool wanted_valid) {
  if (ld(&f->generation) != generation) return false;
  if (!ld(&f->active) || ld(&f->data_gen) != generation) {
    reset_window(f, rtp, generation);
  }

  uint32_t head = ld(&f->head);
  uint32_t tail = ld(&f->tail);
  uint32_t end = rtp + frames;

  /* Entirely before the window: old audio, unless the timeline went back
   * (a block still ahead of the playhead) - then start a new window. */
  if (rtp_delta(end, head) <= 0) {
    if (!(wanted_valid && rtp_delta(rtp, wanted) >= 0)) return true;
    reset_window(f, rtp, generation);
    head = tail = rtp;
  }

  /* Far beyond the window: start over, but never over unplayed audio. */
  if (rtp_delta(rtp, tail) > (int32_t)CAP) {
    if (wanted_valid && rtp_delta(tail, wanted) > 0) return false;
    reset_window(f, rtp, generation);
    head = tail = rtp;
  }

  /* Trim the part before the window. */
  if (rtp_delta(rtp, head) < 0) {
    const uint32_t skip = head - rtp;
    pcm += (size_t)skip * PCM_FIFO_CHANNELS;
    frames -= skip;
    rtp = head;
  }

  /* Room: the window may span at most CAP frames. Drop the oldest frames,
   * but only those the playhead has already passed. */
  if ((uint32_t)rtp_delta(end, head) > CAP) {
    const uint32_t new_head = end - CAP;
    if (wanted_valid && rtp_delta(new_head, wanted) > 0) return false;
    if (rtp_delta(new_head, tail) > 0) {
      reset_window(f, rtp, generation);
      head = tail = rtp;
    } else {
      /* Head-only move: no seq bump. A reader re-checks head after its
       * copy, so it never uses a frame dropped here. */
      __atomic_thread_fence(__ATOMIC_SEQ_CST);
      st(&f->head, new_head);
      __atomic_thread_fence(__ATOMIC_SEQ_CST);
      head = new_head;
    }
  }

  /* Overlap with frames already written: keep them, as a FIFO does, and
   * append only the part after tail. (AutoMix can send a block whose
   * timestamp starts a little before the previous block's end.) */
  if (rtp_delta(rtp, tail) < 0) {
    if (rtp_delta(end, tail) <= 0) return true; /* nothing new */
    pcm += (size_t)(tail - rtp) * PCM_FIFO_CHANNELS;
    rtp = tail;
  }

  /* Gap after tail: a lost block plays as silence. Not yet visible. */
  if (rtp_delta(rtp, tail) > 0) {
    zero_in(f, tail, rtp - tail);
#if defined(CONFIG_AIRPLAY_DIAG_PLAYOUT) && CONFIG_AIRPLAY_DIAG_PLAYOUT
    ++f->gap_events;
    f->gap_frames += rtp - tail;
#endif
  }

  /* Append and publish. */
  if (rtp_delta(end, rtp) > 0) {
    copy_in(f, rtp, pcm, end - rtp);
  }
  if (rtp_delta(end, tail) > 0) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    st(&f->tail, end);
  }
  return true;
}

bool pcm_fifo_write(pcm_fifo_t *f, uint32_t rtp, const pcm_fifo_sample_t *pcm,
                    size_t frames, uint32_t generation, uint32_t wanted_rtp,
                    bool wanted_valid) {
  if (!f || !pcm || frames == 0U || frames > MAX_WRITE) return false;
  FIFO_LOCK(f->mutex);
  const bool ok = write_locked(f, rtp, pcm, (uint32_t)frames, generation,
                               wanted_rtp, wanted_valid);
  FIFO_UNLOCK(f->mutex);
  return ok;
}

uint32_t pcm_fifo_read(const pcm_fifo_t *f, uint32_t rtp, uint32_t frames,
                       uint32_t generation, pcm_fifo_sample_t *out) {
  if (!out || frames == 0U) return 0U;
  if (!f) {
    memset(out, 0, (size_t)frames * FRAME_SZ);
    return 0U;
  }
  const uint32_t end = rtp + frames;
  for (int attempt = 0; attempt < READ_TRIES; ++attempt) {
    const uint32_t s1 = ld(&f->seq);
    if (s1 & 1U) {
      /* A writer is inside a short critical section: wait for it. */
      FIFO_SPIN_US(READ_WAIT_US);
      continue;
    }
    if (!window_current(f, generation)) break;
    const uint32_t head = ld(&f->head);
    const uint32_t tail = ld(&f->tail);
    const uint32_t lo = rtp_delta(head, rtp) > 0 ? head : rtp;
    const uint32_t hi = rtp_delta(end, tail) > 0 ? tail : end;
    if (rtp_delta(hi, lo) <= 0) break;

    if (lo != rtp) memset(out, 0, (size_t)(lo - rtp) * FRAME_SZ);
    copy_out(f, lo, out + (size_t)(lo - rtp) * PCM_FIFO_CHANNELS, hi - lo);
    if (hi != end) {
      memset(out + (size_t)(hi - rtp) * PCM_FIFO_CHANNELS, 0,
             (size_t)(end - hi) * FRAME_SZ);
    }

    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    /* Valid if nothing changed in place and no copied frame was dropped
     * (a dropped frame's memory may already hold newer audio). */
    if (ld(&f->seq) == s1 && rtp_delta(lo, ld(&f->head)) >= 0) {
      return hi - lo;
    }
    FIFO_SPIN_US(READ_WAIT_US);
  }
  memset(out, 0, (size_t)frames * FRAME_SZ);
  return 0U;
}

bool pcm_fifo_has_range(const pcm_fifo_t *f, uint32_t rtp, uint32_t frames,
                        uint32_t generation) {
  if (!f || !window_current(f, generation)) return false;
  return rtp_delta(rtp, ld(&f->head)) >= 0 &&
         rtp_delta(rtp + frames, ld(&f->tail)) <= 0;
}

uint32_t pcm_fifo_contiguous_frames(const pcm_fifo_t *f, uint32_t rtp,
                                    uint32_t generation, uint32_t max_frames) {
  if (!f || !window_current(f, generation)) return 0U;
  const uint32_t head = ld(&f->head);
  const uint32_t tail = ld(&f->tail);
  if (rtp_delta(rtp, head) < 0 || rtp_delta(tail, rtp) <= 0) return 0U;
  const uint32_t n = tail - rtp;
  return n < max_frames ? n : max_frames;
}

void pcm_fifo_invalidate_before(pcm_fifo_t *f, uint32_t until_rtp,
                                uint32_t generation) {
  if (!f) return;
  FIFO_LOCK(f->mutex);
  if (window_current(f, generation) &&
      rtp_delta(until_rtp, ld(&f->head)) > 0) {
    const uint32_t tail = ld(&f->tail);
    /* Past the newest frame: the window becomes empty at until_rtp. */
    set_window(f, until_rtp,
               rtp_delta(until_rtp, tail) >= 0 ? until_rtp : tail);
  }
  FIFO_UNLOCK(f->mutex);
}

void pcm_fifo_truncate(pcm_fifo_t *f, uint32_t from_rtp, uint32_t generation) {
  if (!f) return;
  FIFO_LOCK(f->mutex);
  if (window_current(f, generation)) {
    const uint32_t head = ld(&f->head);
    const uint32_t tail = ld(&f->tail);
    if (rtp_delta(tail, from_rtp) > 0) {
      set_window(f, head, rtp_delta(from_rtp, head) > 0 ? from_rtp : head);
    }
  }
  FIFO_UNLOCK(f->mutex);
}
