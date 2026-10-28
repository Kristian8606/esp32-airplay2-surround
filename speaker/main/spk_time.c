#include "spk_time.h"

#include <inttypes.h>
#include <string.h>

#include "airplay_log.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "ptp_clock.h"
#include "tsf_fit.h"

static const char *TAG = "spk_time";

#define SPK_TIME_BSSID_EVERY  16U /* TIME messages between BSSID reads */
#define SPK_TIME_WARN_MS      10000
#define SPK_TIME_LOG_MS       10000
/* The Gateway's instant lies in the past by the message's transport time.
 * Outside this window the two TSF readings do not belong together. */
#define SPK_TIME_MIN_DELAY_NS (-5000000LL)
#define SPK_TIME_MAX_DELAY_NS 2000000000LL

static tsf_fit_t s_fit;
static bool s_published;
static uint8_t s_bssid[6];
static bool s_bssid_known;
static uint32_t s_msgs;
static int64_t s_warn_us;
static int64_t s_log_us;
static int64_t s_delay_min_ns = INT64_MAX;
static int64_t s_delay_max_ns = INT64_MIN;

static bool warn_now(int64_t now_us) {
  if (s_warn_us != 0 && now_us - s_warn_us < SPK_TIME_WARN_MS * 1000LL)
    return false;
  s_warn_us = now_us;
  return true;
}

static void withdraw(const char *why) {
  if (!s_published) return;
  s_published = false;
  ptp_clock_set_external(NULL);
  AP_LOGI(TAG, "sender time withdrawn: %s", why);
}

void spk_time_reset(void) { withdraw("link to the Gateway lost"); }

bool spk_time_status(uint32_t *tsf_rms_ns) {
  if (tsf_rms_ns) *tsf_rms_ns = (uint32_t)s_fit.rms_ns;
  return s_published;
}

void spk_time_handle(const surround_time_t *m) {
  const int64_t now_us = esp_timer_get_time();
  int64_t local_ns = 0, tsf_ns = 0;
  if (!tsf_fit_read_sample(&local_ns, &tsf_ns)) return;

  if (!s_bssid_known || (s_msgs % SPK_TIME_BSSID_EVERY) == 0U) {
    wifi_ap_record_t ap;
    s_bssid_known = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    if (s_bssid_known) memcpy(s_bssid, ap.bssid, sizeof(s_bssid));
  }
  s_msgs++;
  if (!s_bssid_known || memcmp(m->bssid, s_bssid, sizeof(s_bssid)) != 0) {
    /* Each access point (also each node of a mesh) has its own TSF. */
    if (warn_now(now_us)) {
      AP_LOGW(TAG,
              "Gateway is on access point %02x:%02x:%02x:%02x:%02x:%02x, this "
              "speaker on %02x:%02x:%02x:%02x:%02x:%02x: no common TSF, no "
              "sender time",
              m->bssid[0], m->bssid[1], m->bssid[2], m->bssid[3], m->bssid[4],
              m->bssid[5], s_bssid[0], s_bssid[1], s_bssid[2], s_bssid[3],
              s_bssid[4], s_bssid[5]);
    }
    withdraw("different access point");
    tsf_fit_reset(&s_fit);
    return;
  }

  if (tsf_fit_add(&s_fit, local_ns, tsf_ns) == TSF_FIT_RESTARTED) {
    AP_LOGW(TAG, "TSF stepped (access point restarted or changed): time fit "
                 "restarted");
  }
  if ((m->flags & SURROUND_TIME_PTP_VALID) == 0U) {
    withdraw("the Gateway has no sender time");
    return;
  }
  int64_t at_ns = 0;
  if (!tsf_fit_local_at(&s_fit, m->tsf_ns, &at_ns)) return;
  const int64_t delay_ns = local_ns - at_ns;
  if (delay_ns < SPK_TIME_MIN_DELAY_NS || delay_ns > SPK_TIME_MAX_DELAY_NS) {
    if (warn_now(now_us)) {
      AP_LOGW(TAG, "TIME for %+.1f ms from now: the Gateway's TSF and ours "
                   "disagree, ignored",
              (double)-delay_ns / 1e6);
    }
    return;
  }

  const uint32_t fit_age_ms = tsf_fit_age_ms(&s_fit, local_ns);
  const ptp_clock_external_t ext = {
      .source_clock_id = m->source_clock_id,
      .grandmaster_clock_id = m->grandmaster_clock_id,
      .offset_ns = m->ptp_ns - at_ns,
      .sample_time_ns = at_ns,
      .mastership_age_ms = m->mastership_age_ms < fit_age_ms
                               ? m->mastership_age_ms : fit_age_ms,
      .sample_count = m->sample_count,
      .locked = (m->flags & SURROUND_TIME_PTP_LOCKED) != 0U,
      .realtime = (m->flags & SURROUND_TIME_REALTIME) != 0U,
  };
  ptp_clock_set_external(&ext);
  if (!s_published) {
    AP_LOGI(TAG, "sender time from the Gateway: gm=%016" PRIx64
                 " offset=%" PRId64 " ns",
            m->grandmaster_clock_id, ext.offset_ns);
  }
  s_published = true;

  if (delay_ns < s_delay_min_ns) s_delay_min_ns = delay_ns;
  if (delay_ns > s_delay_max_ns) s_delay_max_ns = delay_ns;
  if (now_us - s_log_us >= SPK_TIME_LOG_MS * 1000LL) {
    s_log_us = now_us;
    ESP_LOGI(TAG,
             "TIME tsf fit %u samples %+.3f ppm rms %.2f us restarts %u | "
             "delay %.1f..%.1f ms | offset=%" PRId64 " ns gm=%016" PRIx64
             " locked=%d",
             (unsigned)s_fit.count, s_fit.slope * 1e6, s_fit.rms_ns / 1000.0,
             (unsigned)s_fit.restarts, (double)s_delay_min_ns / 1e6,
             (double)s_delay_max_ns / 1e6, ext.offset_ns,
             m->grandmaster_clock_id, ext.locked);
    s_delay_min_ns = INT64_MAX;
    s_delay_max_ns = INT64_MIN;
  }
}
