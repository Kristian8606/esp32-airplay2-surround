#include "ptp_clock.h"

#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define CARRY_NS 30000000000LL
#define CARRY_MS 30000U

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static ptp_clock_external_t s_time;
static bool s_valid;
static uint32_t s_epoch, s_gm_changes;

esp_err_t ptp_clock_init_external(void) {
  taskENTER_CRITICAL(&s_mux);
  memset(&s_time, 0, sizeof(s_time));
  s_valid = false;
  s_epoch = s_gm_changes = 0;
  taskEXIT_CRITICAL(&s_mux);
  return ESP_OK;
}

void ptp_clock_set_external(const ptp_clock_external_t *ext) {
  taskENTER_CRITICAL(&s_mux);
  if (!ext) {
    const bool realtime = s_time.realtime;
    s_valid = false;
    s_epoch = 0;
    memset(&s_time, 0, sizeof(s_time));
    s_time.realtime = realtime;
  } else {
    if (s_time.realtime != ext->realtime) {
      s_valid = false;
      s_epoch = s_gm_changes = 0;
      memset(&s_time, 0, sizeof(s_time));
    }
    if (!s_valid || s_time.source_clock_id != ext->source_clock_id ||
        s_time.grandmaster_clock_id != ext->grandmaster_clock_id) {
      if (ext->realtime && s_time.grandmaster_clock_id &&
          s_time.grandmaster_clock_id != ext->grandmaster_clock_id)
        s_gm_changes++;
      if (++s_epoch == 0) s_epoch = 1;
    }
    s_time = *ext;
    s_valid = true;
  }
  taskEXIT_CRITICAL(&s_mux);
}

static uint32_t age_ms(int64_t now, int64_t then) {
  if (then <= 0 || now < then) return UINT32_MAX;
  const uint64_t ms = (uint64_t)(now-then) / 1000000ULL;
  return ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

void ptp_clock_get_snapshot(ptp_clock_snapshot_t *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  const int64_t now = esp_timer_get_time() * 1000LL;
  taskENTER_CRITICAL(&s_mux);
  const ptp_clock_external_t t = s_time;
  const bool valid = s_valid;
  const uint32_t epoch = s_epoch, changes = s_gm_changes;
  taskEXIT_CRITICAL(&s_mux);
  const uint32_t age = age_ms(now, t.sample_time_ns);
  out->sample_age_ms = age == UINT32_MAX ? age : age > CARRY_MS ? age-CARRY_MS : 0U;
  out->mastership_age_ms = valid
      ? age_ms(now, t.sample_time_ns - (int64_t)t.mastership_age_ms * 1000000LL) : 0U;
  if (out->mastership_age_ms == UINT32_MAX) out->mastership_age_ms = 0;
  out->realtime_mode = t.realtime;
  out->locked = t.realtime
      ? valid && t.grandmaster_clock_id && t.sample_count >= 4 &&
        out->mastership_age_ms >= 400 && out->sample_age_ms <= 2000
      : valid && t.locked && age <= CARRY_MS+5000U;
  out->valid = t.realtime ? out->locked : valid;
  out->source_clock_id = t.source_clock_id;
  out->grandmaster_clock_id = t.grandmaster_clock_id;
  out->epoch = t.realtime ? (t.grandmaster_clock_id ? changes+1U : 0U) : epoch;
  out->raw_offset_ns = t.offset_ns;
  int64_t dt = now > t.sample_time_ns ? now-t.sample_time_ns : 0;
  if (dt > CARRY_NS) dt = CARRY_NS;
  out->filtered_offset_ns = t.offset_ns + dt*t.offset_rate_ppb/1000000000LL;
  out->raw_filter_delta_ns = out->raw_offset_ns - out->filtered_offset_ns;
  out->sample_count = t.sample_count;
  out->offset_rate_valid = valid;
  out->offset_rate_ppb = t.offset_rate_ppb;
}

void ptp_clock_get_realtime_snapshot(ptp_realtime_snapshot_t *out) {
  if (!out) return;
  ptp_clock_snapshot_t snap;
  ptp_clock_get_snapshot(&snap);
  *out = (ptp_realtime_snapshot_t){.realtime_mode = snap.realtime_mode,
      .master_ready = snap.realtime_mode && snap.locked,
      .master_clock_id = snap.grandmaster_clock_id,
      .source_clock_id = snap.source_clock_id,
      .master_offset_ns = snap.filtered_offset_ns,
      .mastership_age_ms = snap.mastership_age_ms,
      .sample_count = snap.sample_count, .sample_age_ms = snap.sample_age_ms,
      .gm_change_count = snap.epoch ? snap.epoch-1U : 0U};
}

bool ptp_clock_realtime_snapshot_to_local(const ptp_realtime_snapshot_t *snap,
    uint64_t id, uint64_t remote, uint64_t *local) {
  return snap && snap->realtime_mode && snap->master_ready && id &&
      id == snap->master_clock_id && remote <= INT64_MAX &&
      ptp_clock_remote_to_local(remote, snap->master_offset_ns, local) &&
      *local <= INT64_MAX;
}

static bool add_signed_u64(uint64_t base, int64_t delta, uint64_t *out) {
  if (!out) return false;
  if (delta >= 0) {
    const uint64_t add = (uint64_t)delta;
    if (base > UINT64_MAX - add) return false;
    *out = base + add;
  } else {
    const uint64_t sub = (uint64_t)(-(delta + 1)) + 1U;
    if (base < sub) return false;
    *out = base - sub;
  }
  return true;
}

bool ptp_clock_remote_to_local(uint64_t remote_ns, int64_t offset_ns,
                                      uint64_t *local_ns) {
  if (offset_ns == INT64_MIN) {
    if (!local_ns || remote_ns > UINT64_MAX - ((uint64_t)INT64_MAX + 1ULL))
      return false;
    *local_ns = remote_ns + ((uint64_t)INT64_MAX + 1ULL);
    return true;
  }
  return add_signed_u64(remote_ns, -offset_ns, local_ns);
}

bool ptp_clock_local_to_remote(uint64_t local_ns, int64_t offset_ns,
                                      uint64_t *remote_ns) {
  return add_signed_u64(local_ns, offset_ns, remote_ns);
}

bool ptp_clock_handover_ready(bool have_local_anchor, bool clock_locked,
                                     uint64_t anchor_clock_id,
                                     uint64_t current_master_clock_id,
                                     uint32_t mastership_age_ms,
                                     uint32_t last_anchor_age_ms) {
  return have_local_anchor && clock_locked && anchor_clock_id != 0 &&
         current_master_clock_id != 0 &&
         anchor_clock_id != current_master_clock_id &&
         mastership_age_ms >= 400U && last_anchor_age_ms >= 5000U;
}
