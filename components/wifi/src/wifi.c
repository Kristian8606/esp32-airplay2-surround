/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wifi.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "dns_server.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "settings.h"
#include "wifi_ctl.h"
#include "wifi_logic.h"
#include "wifi_saved.h"

static const char *TAG = "wifi";

#define TIMER_POST_RETRY_US (100 * 1000)
#define RESUME_POST_TIMEOUT_MS 1000
#define DEVICE_NAME_BUF_LEN 128

#define BIT_CONNECTED BIT0
#define BIT_USER_SCAN_DONE BIT1

/* Our own events, so that the timers and wifi_scan() hand their work to the
 * default event loop task, where the state machine runs. */
static ESP_EVENT_DEFINE_BASE(WIFI_MODULE_EVENT);
enum {
  EV_RETRY_TIMER,
  EV_AP_TIMER,
  EV_DHCP_TIMER,
  EV_SCAN_TIMER,
  EV_RESUME,
};

/* The saved networks, copied once in wifi_init_apsta() (saving a network
 * restarts the board, so the copy never goes stale), and the large buffers
 * of the event loop task. One PSRAM block for the module's life, so that
 * the handlers keep small stack frames and never touch settings or NVS. */
typedef struct {
  settings_wifi_network_t net[SETTINGS_MAX_WIFI_NETWORKS];
  wl_saved_t saved[SETTINGS_MAX_WIFI_NETWORKS]; /* views of net[] */
  size_t count;
  wifi_config_t cfg;        /* event loop task only */
  wifi_ap_record_t ap_info; /* event loop task only */
} store_t;

typedef enum {
  OWNER_NONE,
  OWNER_STA,  /* the state machine: own scan or attempt */
  OWNER_USER, /* wifi_scan() */
} owner_t;

static struct {
  atomic_bool init_called;
  atomic_bool started;
  esp_netif_t *sta_netif;
  EventGroupHandle_t events;
  StaticEventGroup_t events_buf;
  SemaphoreHandle_t op_sem; /* one scan or attempt at a time */
  StaticSemaphore_t op_sem_buf;
  esp_timer_handle_t retry_timer;
  esp_timer_handle_t ap_timer;
  esp_timer_handle_t dhcp_timer;
  esp_timer_handle_t scan_timer;

  /* Read by other tasks, under lock. */
  portMUX_TYPE lock;
  bool connected;
  uint32_t ip; /* network byte order */
  owner_t owner;
  /* Every scan of this module gets a number; 0: none runs. A SCAN_DONE
   * belongs to the scan that runs, and none that comes while none runs. */
  uint32_t scan_seq;      /* the scan running now */
  uint32_t last_scan_seq; /* the number given last */
  uint32_t user_done_seq; /* the wifi_scan() scan whose SCAN_DONE came */
  uint32_t user_scan_status;

  /* Event loop task only (and wifi_init_apsta() before Wi-Fi starts). */
  wl_ctl_t ctl;
  unsigned ap_clients;
  wl_ap_params_t ap;
  store_t *store; /* set in wifi_init_apsta(), never freed */
} s = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

/* An error in start-up is a fault of the build or the hardware. */
#define INIT_CHECK(x)                                              \
  do {                                                             \
    esp_err_t err_ = (x);                                          \
    if (err_ != ESP_OK) {                                          \
      ESP_LOGE(TAG, "%s failed: %s", #x, esp_err_to_name(err_));   \
      ESP_ERROR_CHECK(err_);                                       \
    }                                                              \
  } while (0)

#define MAC_FMT "%02x:%02x:%02x:%02x:%02x:%02x"
#define MAC_ARGS(m) (m)[0], (m)[1], (m)[2], (m)[3], (m)[4], (m)[5]

static void *alloc_psram(size_t size) {
  void *p = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p != NULL ? p : calloc(1, size);
}

static void set_owner(owner_t owner) {
  taskENTER_CRITICAL(&s.lock);
  s.owner = owner;
  taskEXIT_CRITICAL(&s.lock);
}

static owner_t get_owner(void) {
  taskENTER_CRITICAL(&s.lock);
  owner_t owner = s.owner;
  taskEXIT_CRITICAL(&s.lock);
  return owner;
}

/* Starts a scan of all channels under a new number; returns the number
 * (never 0) or 0 when ESP-IDF did not start it (*err says why). */
static uint32_t scan_start(esp_err_t *err);

/* The scan that runs now ends (SCAN_DONE, or given up): returns its number,
 * 0 when none ran. */
static uint32_t scan_end(void) {
  taskENTER_CRITICAL(&s.lock);
  uint32_t seq = s.scan_seq;
  s.scan_seq = 0;
  taskEXIT_CRITICAL(&s.lock);
  return seq;
}

static void set_connected(bool connected, uint32_t ip) {
  taskENTER_CRITICAL(&s.lock);
  s.connected = connected;
  s.ip = connected ? ip : 0;
  taskEXIT_CRITICAL(&s.lock);
  if (connected) {
    xEventGroupSetBits(s.events, BIT_CONNECTED);
  } else {
    xEventGroupClearBits(s.events, BIT_CONNECTED);
  }
}

static wl_sec_t sec_of(wifi_auth_mode_t mode) {
  switch (mode) {
    case WIFI_AUTH_OPEN:
      return WL_SEC_OPEN;
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA_WPA2_PSK:
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA2_WPA3_PSK:
      return WL_SEC_PSK;
    case WIFI_AUTH_OWE: /* not open: a saved "" never joins it */
    default:            /* WPA alone, WEP, enterprise, WAPI, DPP */
      return WL_SEC_OTHER;
  }
}

static void to_views(const wifi_ap_record_t *recs, size_t n, wl_ap_t *views) {
  for (size_t i = 0; i < n; i++) {
    views[i].ssid = recs[i].ssid;
    views[i].rssi = recs[i].rssi;
    memcpy(views[i].bssid, recs[i].bssid, sizeof(views[i].bssid));
    views[i].channel = recs[i].primary;
    views[i].sec = sec_of(recs[i].authmode);
  }
}

/* The records of the scan that ended, in PSRAM if possible. Frees the
 * driver's list in every case. */
static esp_err_t take_scan_records(wifi_ap_record_t **recs, uint16_t *count) {
  *recs = NULL;
  *count = 0;
  uint16_t n = 0;
  esp_err_t err = esp_wifi_scan_get_ap_num(&n);
  if (err != ESP_OK || n == 0) {
    esp_wifi_clear_ap_list();
    return err;
  }
  wifi_ap_record_t *r = alloc_psram((size_t)n * sizeof(*r));
  if (r == NULL) {
    esp_wifi_clear_ap_list();
    return ESP_ERR_NO_MEM;
  }
  err = esp_wifi_scan_get_ap_records(&n, r);
  if (err != ESP_OK) {
    free(r);
    esp_wifi_clear_ap_list();
    return err;
  }
  *recs = r;
  *count = n;
  return ESP_OK;
}

static const wifi_scan_config_t scan_config = {
    .ssid = NULL,
    .bssid = NULL,
    .channel = 0, /* all channels */
    .show_hidden = false,
    .scan_type = WIFI_SCAN_TYPE_ACTIVE,
};

static uint32_t scan_start(esp_err_t *err) {
  taskENTER_CRITICAL(&s.lock);
  uint32_t seq = ++s.last_scan_seq;
  if (seq == 0) {
    seq = ++s.last_scan_seq;
  }
  s.scan_seq = seq; /* before the start: SCAN_DONE may come at once */
  taskEXIT_CRITICAL(&s.lock);
  *err = esp_wifi_scan_start(&scan_config, false);
  if (*err != ESP_OK) {
    scan_end();
    return 0;
  }
  return seq;
}

/* ---- What the state machine asks for ---------------------------------- */

static bool op_try_acquire(void *ctx) {
  (void)ctx;
  if (xSemaphoreTake(s.op_sem, 0) != pdTRUE) {
    return false;
  }
  set_owner(OWNER_STA);
  return true;
}

static void op_release(void *ctx) {
  (void)ctx;
  set_owner(OWNER_NONE);
  xSemaphoreGive(s.op_sem);
}

static bool op_start_scan(void *ctx) {
  (void)ctx;
  esp_err_t err;
  if (scan_start(&err) == 0) {
    ESP_LOGW(TAG, "scan did not start: %s", esp_err_to_name(err));
    return false;
  }
  ESP_LOGI(TAG, "scanning for saved networks");
  return true;
}

static bool op_connect(void *ctx, const wl_target_t *t) {
  (void)ctx;
  if (t->saved_index >= s.store->count) {
    ESP_LOGE(TAG, "attempt: no saved network %u", (unsigned)t->saved_index);
    return false;
  }
  const settings_wifi_network_t *net = &s.store->net[t->saved_index];
  wifi_config_t *cfg = &s.store->cfg;
  memset(cfg, 0, sizeof(*cfg));
  size_t ssid_len = wl_ssid_len(net->ssid);
  size_t pw_len = strnlen(net->password, sizeof(cfg->sta.password));
  wifi_sta_config_t *sta = &cfg->sta;
  memcpy(sta->ssid, net->ssid, ssid_len);
  memcpy(sta->password, net->password, pw_len);
  sta->scan_method = t->by_bssid ? WIFI_FAST_SCAN : WIFI_ALL_CHANNEL_SCAN;
  sta->sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  sta->bssid_set = t->by_bssid;
  if (t->by_bssid) {
    memcpy(sta->bssid, t->bssid, sizeof(sta->bssid));
    sta->channel = t->channel;
  }
  /* "" joins only an open network; a password at least WPA2-PSK (so never
   * open or WEP), WPA3-SAE too, PMF capable but not required. */
  sta->threshold.authmode = pw_len > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
  sta->pmf_cfg.capable = true;
  sta->pmf_cfg.required = false;
  sta->sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
  /* No 802.11k/v/r, no MBO: the station stays on its access point. */
  sta->rm_enabled = 0;
  sta->btm_enabled = 0;
  sta->mbo_enabled = 0;
  sta->ft_enabled = 0;
  sta->owe_enabled = 0;
  sta->failure_retry_cnt = 0;

  esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, cfg);
  memset(cfg, 0, sizeof(*cfg)); /* no password left in the scratch copy */
  if (err == ESP_OK) {
    err = esp_wifi_connect();
  }
  if (t->by_bssid) {
    ESP_LOGI(TAG, "attempt: SSID \"%.*s\" BSSID " MAC_FMT " channel %u%s%s",
             (int)ssid_len, net->ssid, MAC_ARGS(t->bssid), t->channel,
             err == ESP_OK ? "" : " failed: ",
             err == ESP_OK ? "" : esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "attempt: SSID \"%.*s\" by SSID alone (any BSSID, all "
             "channels)%s%s",
             (int)ssid_len, net->ssid, err == ESP_OK ? "" : " failed: ",
             err == ESP_OK ? "" : esp_err_to_name(err));
  }
  return err == ESP_OK;
}

static void timer_arm(esp_timer_handle_t timer, uint32_t ms) {
  esp_timer_stop(timer); /* ESP_ERR_INVALID_STATE when not running: fine */
  esp_err_t err = esp_timer_start_once(timer, (uint64_t)ms * 1000);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "timer: %s", esp_err_to_name(err));
  }
}

static void op_arm_retry(void *ctx, uint32_t ms) {
  (void)ctx;
  esp_timer_stop(s.retry_timer);
  if (ms == 0) {
    /* "At once": through the event queue, so that the state machine never
     * calls itself again on this stack. */
    if (esp_event_post(WIFI_MODULE_EVENT, EV_RETRY_TIMER, NULL, 0, 0) ==
        ESP_OK) {
      return;
    }
    ms = 1; /* queue full: the timer instead */
  }
  if (ms >= 1000) {
    ESP_LOGI(TAG, "next attempt in %u s", (unsigned)(ms / 1000));
  }
  timer_arm(s.retry_timer, ms);
}

static void op_cancel_retry(void *ctx) {
  (void)ctx;
  esp_timer_stop(s.retry_timer);
}

static void op_arm_ap_timer(void *ctx, uint32_t ms) {
  (void)ctx;
  timer_arm(s.ap_timer, ms);
}

static void op_cancel_ap_timer(void *ctx) {
  (void)ctx;
  esp_timer_stop(s.ap_timer);
}

static void op_stop_scan(void *ctx) {
  (void)ctx;
  ESP_LOGW(TAG, "scan for saved networks: no result in %u s: stopping it",
           (unsigned)(WL_SCAN_TIMEOUT_MS / 1000));
  esp_wifi_scan_stop();
}

static void op_end_scan(void *ctx) {
  (void)ctx;
  scan_end(); /* a SCAN_DONE after this is ignored */
  esp_wifi_clear_ap_list();
}

static void op_arm_scan_timer(void *ctx, uint32_t ms) {
  (void)ctx;
  timer_arm(s.scan_timer, ms);
}

static void op_cancel_scan_timer(void *ctx) {
  (void)ctx;
  esp_timer_stop(s.scan_timer);
}

static void op_arm_dhcp_timer(void *ctx, uint32_t ms) {
  (void)ctx;
  timer_arm(s.dhcp_timer, ms);
}

static void op_cancel_dhcp_timer(void *ctx) {
  (void)ctx;
  esp_timer_stop(s.dhcp_timer);
}

static bool op_disconnect(void *ctx) {
  (void)ctx;
  esp_err_t err = esp_wifi_disconnect();
  ESP_LOGW(TAG, "no address %u s after joining: disconnecting%s%s",
           (unsigned)(WL_DHCP_TIMEOUT_MS / 1000),
           err == ESP_OK ? "" : "; failed: ",
           err == ESP_OK ? "" : esp_err_to_name(err));
  return err == ESP_OK;
}

static void ap_config(wifi_config_t *cfg) {
  memset(cfg, 0, sizeof(*cfg));
  wifi_ap_config_t *ap = &cfg->ap;
  memcpy(ap->ssid, s.ap.ssid, s.ap.ssid_len);
  ap->ssid_len = s.ap.ssid_len;
  memcpy(ap->password, s.ap.password, s.ap.password_len);
  ap->authmode = s.ap.wpa2 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
  ap->channel = 1; /* follows the station's channel while it is connected */
  ap->max_connection = WL_AP_MAX_CLIENTS;
  ap->beacon_interval = 100;
  ap->pmf_cfg.required = false;
}

static void op_set_ap(void *ctx, bool on, wl_ap_why_t why) {
  (void)ctx;
  esp_err_t err;
  if (on) {
    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
      wifi_config_t *cfg = &s.store->cfg;
      ap_config(cfg);
      err = esp_wifi_set_config(WIFI_IF_AP, cfg);
      memset(cfg, 0, sizeof(*cfg));
    }
    ESP_LOGI(TAG, "access point on: SSID \"%.*s\" (%s), %s%s%s",
             (int)s.ap.ssid_len, (const char *)s.ap.ssid,
             s.ap.wpa2 ? "WPA2-PSK" : "open",
             why == WL_AP_WHY_NO_SAVED ? "no saved network"
                                       : "no address for 30 s",
             err == ESP_OK ? "" : "; failed: ",
             err == ESP_OK ? "" : esp_err_to_name(err));
  } else {
    s.ap_clients = 0;
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "access point off%s%s", err == ESP_OK ? "" : "; failed: ",
             err == ESP_OK ? "" : esp_err_to_name(err));
  }
}

static bool op_dns_start(void *ctx) {
  (void)ctx;
  esp_err_t err = dns_server_start(wl_ipv4(192, 168, 4, 1));
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "captive DNS did not start: %s", esp_err_to_name(err));
    return false;
  }
  ESP_LOGI(TAG, "captive DNS on");
  return true;
}

static void op_dns_stop(void *ctx) {
  (void)ctx;
  dns_server_stop();
  ESP_LOGI(TAG, "captive DNS off");
}

static const wl_ctl_ops_t ctl_ops = {
    .ctx = NULL,
    .try_acquire = op_try_acquire,
    .release = op_release,
    .start_scan = op_start_scan,
    .stop_scan = op_stop_scan,
    .end_scan = op_end_scan,
    .arm_scan_timer = op_arm_scan_timer,
    .cancel_scan_timer = op_cancel_scan_timer,
    .connect = op_connect,
    .arm_retry = op_arm_retry,
    .cancel_retry = op_cancel_retry,
    .arm_ap_timer = op_arm_ap_timer,
    .cancel_ap_timer = op_cancel_ap_timer,
    .arm_dhcp_timer = op_arm_dhcp_timer,
    .cancel_dhcp_timer = op_cancel_dhcp_timer,
    .disconnect = op_disconnect,
    .set_ap = op_set_ap,
    .dns_start = op_dns_start,
    .dns_stop = op_dns_stop,
};

/* ---- Events ----------------------------------------------------------- */

static void timer_cb(void *arg) {
  int32_t id = (int32_t)(intptr_t)arg;
  if (esp_event_post(WIFI_MODULE_EVENT, id, NULL, 0, 0) != ESP_OK) {
    /* Event queue full: try again soon rather than lose the timer. */
    esp_timer_handle_t t = id == EV_RETRY_TIMER ? s.retry_timer
                           : id == EV_AP_TIMER  ? s.ap_timer
                           : id == EV_DHCP_TIMER ? s.dhcp_timer
                                                 : s.scan_timer;
    esp_timer_start_once(t, TIMER_POST_RETRY_US);
  }
}

/* The state machine's own scan ended. */
static void sta_scan_done(void) {
  wifi_ap_record_t *recs = NULL;
  uint16_t n = 0;
  esp_err_t err = take_scan_records(&recs, &n);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "scan results: %s", esp_err_to_name(err));
  }

  wl_ap_t *views = n > 0 ? alloc_psram((size_t)n * sizeof(*views)) : NULL;
  if (n > 0 && views == NULL) {
    ESP_LOGE(TAG, "scan results: no memory");
    n = 0;
  } else {
    to_views(recs, n, views);
  }

  ESP_LOGI(TAG, "scan done: %u access points", (unsigned)n);

  wl_ctl_on_scan_done(&s.ctl, views, n, s.store->saved, s.store->count);

  free(views);
  free(recs);
}

static void on_wifi_event(int32_t id, void *data) {
  switch (id) {
    case WIFI_EVENT_STA_START:
      wl_ctl_start(&s.ctl);
      break;

    case WIFI_EVENT_SCAN_DONE: {
      const wifi_event_sta_scan_done_t *e = data;
      uint32_t seq = scan_end();
      if (seq == 0) {
        /* No scan of ours runs (a late one after esp_wifi_scan_stop()). */
        ESP_LOGD(TAG, "SCAN_DONE without a scan: ignored");
        break;
      }
      owner_t owner = get_owner();
      if (owner == OWNER_USER) {
        taskENTER_CRITICAL(&s.lock);
        s.user_scan_status = e != NULL ? e->status : 1;
        s.user_done_seq = seq;
        taskEXIT_CRITICAL(&s.lock);
        xEventGroupSetBits(s.events, BIT_USER_SCAN_DONE);
      } else if (owner == OWNER_STA && wl_ctl_wants_scan(&s.ctl)) {
        sta_scan_done();
      } else {
        /* The station's scan that was stopped after 10 s. */
        esp_wifi_clear_ap_list();
        wl_ctl_on_scan_done(&s.ctl, NULL, 0, NULL, 0);
      }
      break;
    }

    case WIFI_EVENT_AP_STACONNECTED:
      s.ap_clients++;
      ESP_LOGI(TAG, "access point: client joined (%u)", s.ap_clients);
      wl_ctl_on_ap_clients(&s.ctl, s.ap_clients);
      break;

    case WIFI_EVENT_AP_STADISCONNECTED:
      if (s.ap_clients > 0) {
        s.ap_clients--;
      }
      ESP_LOGI(TAG, "access point: client left (%u)", s.ap_clients);
      wl_ctl_on_ap_clients(&s.ctl, s.ap_clients);
      break;

    case WIFI_EVENT_AP_STOP:
      s.ap_clients = 0;
      wl_ctl_on_ap_clients(&s.ctl, 0);
      break;

    case WIFI_EVENT_STA_CONNECTED: {
      const wifi_event_sta_connected_t *e = data;
      ESP_LOGI(TAG, "associated: SSID \"%.*s\" BSSID " MAC_FMT " channel %u",
               (int)(e->ssid_len <= 32 ? e->ssid_len : 32), e->ssid,
               MAC_ARGS(e->bssid), e->channel);
      wl_ctl_on_connected(&s.ctl, e->bssid, e->channel);
      break;
    }

    case WIFI_EVENT_STA_DISCONNECTED: {
      const wifi_event_sta_disconnected_t *e = data;
      ESP_LOGW(TAG,
               "disconnected: SSID \"%.*s\" BSSID " MAC_FMT
               " reason %u (%s) RSSI %d",
               (int)(e->ssid_len <= 32 ? e->ssid_len : 32), e->ssid,
               MAC_ARGS(e->bssid), e->reason, wl_reason_name(e->reason),
               e->rssi);
      set_connected(false, 0);
      wl_ctl_on_disconnected(&s.ctl);
      break;
    }

    default:
      break;
  }
}

static void on_ip_event(int32_t id, void *data) {
  if (id == IP_EVENT_STA_GOT_IP) {
    const ip_event_got_ip_t *e = data;
    uint32_t ip = e->ip_info.ip.addr;
    set_connected(true, ip);
    char ip_str[16];
    wl_ip_str(true, ip, ip_str, sizeof(ip_str));
    wifi_ap_record_t *ap = &s.store->ap_info;
    if (esp_wifi_sta_get_ap_info(ap) == ESP_OK) {
      ESP_LOGI(TAG,
               "connected: SSID \"%.*s\" BSSID " MAC_FMT
               " channel %u RSSI %d address %s",
               (int)wl_ssid_len(ap->ssid), ap->ssid, MAC_ARGS(ap->bssid),
               ap->primary, ap->rssi, ip_str);
    } else {
      ESP_LOGI(TAG, "connected: address %s", ip_str);
    }
    wl_ctl_on_got_ip(&s.ctl);
  } else if (id == IP_EVENT_STA_LOST_IP) {
    ESP_LOGW(TAG, "address lost");
    set_connected(false, 0);
    wl_ctl_on_lost_ip(&s.ctl);
  }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id,
                     void *data) {
  (void)arg;
  if (base == WIFI_EVENT) {
    on_wifi_event(id, data);
  } else if (base == IP_EVENT) {
    on_ip_event(id, data);
  } else if (base == WIFI_MODULE_EVENT) {
    switch (id) {
      case EV_RETRY_TIMER:
        wl_ctl_on_retry_timer(&s.ctl);
        break;
      case EV_AP_TIMER:
        wl_ctl_on_ap_timer(&s.ctl);
        break;
      case EV_DHCP_TIMER:
        wl_ctl_on_dhcp_timer(&s.ctl);
        break;
      case EV_SCAN_TIMER:
        wl_ctl_on_scan_timer(&s.ctl);
        break;
      case EV_RESUME:
        wl_ctl_on_resume(&s.ctl);
        break;
      default:
        break;
    }
  }
}

/* ---- Host name -------------------------------------------------------- */

static void read_sta_mac(uint8_t mac[6]) {
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
    memset(mac, 0, 6);
  }
}

static void apply_hostname(const char *device_name) {
  uint8_t mac[6];
  read_sta_mac(mac);
  char name[WIFI_HOSTNAME_MAX_LEN + 1];
  wl_sanitize_hostname(device_name, mac, name, sizeof(name));
  esp_err_t err = esp_netif_set_hostname(s.sta_netif, name);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "host name \"%s\"", name);
  } else {
    ESP_LOGE(TAG, "host name \"%s\": %s", name, esp_err_to_name(err));
  }
}

/* ---- Interface -------------------------------------------------------- */

void wifi_init_apsta(const char *ap_ssid, const char *ap_password) {
  bool expected = false;
  if (!atomic_compare_exchange_strong(&s.init_called, &expected, true)) {
    ESP_LOGW(TAG, "wifi_init_apsta() called again: ignored");
    return;
  }

  wl_ap_params(ap_ssid, ap_password, &s.ap);
  if (s.ap.bad_ssid) {
    ESP_LOGW(TAG, "access point SSID must be 1 to 32 bytes: using \"%s\"",
             WL_DEFAULT_AP_SSID);
  }
  if (s.ap.bad_password) {
    ESP_LOGW(TAG, "access point password must be 8 to 63 bytes: open "
             "access point");
  }

  /* The only place that reads settings: the event loop task must not. */
  s.store = alloc_psram(sizeof(*s.store));
  if (s.store == NULL) {
    INIT_CHECK(ESP_ERR_NO_MEM);
  }
  s.store->count = wifi_saved_load(s.store->net, s.store->saved);

  s.events = xEventGroupCreateStatic(&s.events_buf);
  s.op_sem = xSemaphoreCreateBinaryStatic(&s.op_sem_buf);
  xSemaphoreGive(s.op_sem);

  esp_err_t err = esp_netif_init();
  if (err != ESP_ERR_INVALID_STATE) {
    INIT_CHECK(err);
  }
  err = esp_event_loop_create_default();
  if (err != ESP_ERR_INVALID_STATE) {
    INIT_CHECK(err);
  }
  s.sta_netif = esp_netif_create_default_wifi_sta();
  esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
  if (s.sta_netif == NULL || ap_netif == NULL) {
    INIT_CHECK(ESP_FAIL);
  }

  /* Before the station starts, so that the first DHCP request has it. */
  char *device_name = calloc(1, DEVICE_NAME_BUF_LEN);
  bool have_name =
      device_name != NULL &&
      settings_get_device_name(device_name, DEVICE_NAME_BUF_LEN) == ESP_OK;
  apply_hostname(have_name ? device_name : NULL);
  free(device_name);

  wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
  INIT_CHECK(esp_wifi_init(&init_cfg));
  INIT_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

  INIT_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                 on_event, NULL, NULL));
  INIT_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL));
  INIT_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_LOST_IP, on_event, NULL, NULL));
  INIT_CHECK(esp_event_handler_instance_register(
      WIFI_MODULE_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL));

  const esp_timer_create_args_t retry_args = {
      .callback = timer_cb,
      .arg = (void *)(intptr_t)EV_RETRY_TIMER,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "wifi_retry",
  };
  const esp_timer_create_args_t ap_args = {
      .callback = timer_cb,
      .arg = (void *)(intptr_t)EV_AP_TIMER,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "wifi_ap",
  };
  INIT_CHECK(esp_timer_create(&retry_args, &s.retry_timer));
  const esp_timer_create_args_t dhcp_args = {
      .callback = timer_cb,
      .arg = (void *)(intptr_t)EV_DHCP_TIMER,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "wifi_dhcp",
  };
  INIT_CHECK(esp_timer_create(&ap_args, &s.ap_timer));
  const esp_timer_create_args_t scan_args = {
      .callback = timer_cb,
      .arg = (void *)(intptr_t)EV_SCAN_TIMER,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "wifi_scan",
  };
  INIT_CHECK(esp_timer_create(&dhcp_args, &s.dhcp_timer));
  INIT_CHECK(esp_timer_create(&scan_args, &s.scan_timer));

  wl_ctl_init(&s.ctl, &ctl_ops, s.store->count);
  ESP_LOGI(TAG, "%u saved networks", (unsigned)s.store->count);

  /* The access point's configuration is set once here (it can only be set
   * while the mode has the access point), so that turning it on later never
   * shows the driver's default access point. Then station only: the state
   * machine turns the access point on when the rules say so. */
  ap_config(&s.store->cfg);
  INIT_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  INIT_CHECK(esp_wifi_set_config(WIFI_IF_AP, &s.store->cfg));
  memset(&s.store->cfg, 0, sizeof(s.store->cfg));
  INIT_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

  /* Power save delays received packets up to a beacon interval. */
  INIT_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
  INIT_CHECK(esp_wifi_start());
  atomic_store(&s.started, true);
}

bool wifi_wait_connected(uint32_t timeout_ms) {
  if (!atomic_load(&s.started)) {
    return false;
  }
  TickType_t ticks = portMAX_DELAY;
  if (timeout_ms != 0) {
    uint64_t t = ((uint64_t)timeout_ms * configTICK_RATE_HZ + 999) / 1000;
    ticks = t >= portMAX_DELAY ? portMAX_DELAY - 1 : (TickType_t)t;
  }
  EventBits_t bits =
      xEventGroupWaitBits(s.events, BIT_CONNECTED, pdFALSE, pdTRUE, ticks);
  return (bits & BIT_CONNECTED) != 0;
}

void wifi_get_mac_str(char *mac_str, size_t len) {
  if (mac_str == NULL || len == 0) {
    return;
  }
  uint8_t mac[6];
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
    mac_str[0] = '\0';
    return;
  }
  wl_mac_str(mac, mac_str, len);
}

bool wifi_is_connected(void) {
  if (!atomic_load(&s.started)) {
    return false;
  }
  taskENTER_CRITICAL(&s.lock);
  bool connected = s.connected;
  taskEXIT_CRITICAL(&s.lock);
  return connected;
}

esp_err_t wifi_get_ip_str(char *ip_str, size_t len) {
  bool connected = false;
  uint32_t ip = 0;
  if (atomic_load(&s.started)) {
    taskENTER_CRITICAL(&s.lock);
    connected = s.connected;
    ip = s.ip;
    taskEXIT_CRITICAL(&s.lock);
  }
  return wl_ip_str(connected, ip, ip_str, len);
}

static TickType_t ticks_until(int64_t deadline_us) {
  int64_t left = deadline_us - esp_timer_get_time();
  if (left <= 0) {
    return 0;
  }
  return pdMS_TO_TICKS((uint32_t)((left + 999) / 1000));
}

static void scan_end_if(uint32_t seq) {
  taskENTER_CRITICAL(&s.lock);
  if (s.scan_seq == seq) {
    s.scan_seq = 0;
  }
  taskEXIT_CRITICAL(&s.lock);
}

/* Waits for the SCAN_DONE of wifi_scan()'s scan number seq. */
static bool wait_user_scan_done(uint32_t seq, int64_t deadline_us) {
  for (;;) {
    EventBits_t bits = xEventGroupWaitBits(s.events, BIT_USER_SCAN_DONE,
                                           pdTRUE, pdTRUE,
                                           ticks_until(deadline_us));
    if ((bits & BIT_USER_SCAN_DONE) == 0) {
      return false;
    }
    taskENTER_CRITICAL(&s.lock);
    bool mine = s.user_done_seq == seq;
    taskEXIT_CRITICAL(&s.lock);
    if (mine) {
      return true;
    }
  }
}

esp_err_t wifi_scan(wifi_ap_record_t **ap_list, uint16_t *ap_count) {
  if (ap_list != NULL) {
    *ap_list = NULL;
  }
  if (ap_count != NULL) {
    *ap_count = 0;
  }
  if (ap_list == NULL || ap_count == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!atomic_load(&s.started)) {
    return ESP_ERR_INVALID_STATE;
  }

  const int64_t deadline =
      esp_timer_get_time() + (int64_t)WL_SCAN_TIMEOUT_MS * 1000;
  /* Waits for the station's own scan or attempt to end. */
  if (xSemaphoreTake(s.op_sem, ticks_until(deadline)) != pdTRUE) {
    ESP_LOGW(TAG, "scan: station busy for 10 s");
    return ESP_ERR_TIMEOUT;
  }
  set_owner(OWNER_USER);
  xEventGroupClearBits(s.events, BIT_USER_SCAN_DONE);

  wifi_ap_record_t *recs = NULL;
  wl_ap_t *views = NULL;
  uint16_t n = 0;
  uint16_t idx[WL_SCAN_LIST_MAX];
  size_t count = 0;
  uint32_t status = 0;
  esp_err_t err;
  const uint32_t seq = scan_start(&err);
  if (seq == 0) {
    ESP_LOGW(TAG, "scan: %s", esp_err_to_name(err));
    goto out;
  }
  if (!wait_user_scan_done(seq, deadline)) {
    ESP_LOGW(TAG, "scan: no result in 10 s: stopping it");
    esp_wifi_scan_stop();
    /* Keep the right until its SCAN_DONE or 1 s more. */
    const int64_t grace =
        esp_timer_get_time() + (int64_t)WL_SCAN_STOP_GRACE_MS * 1000;
    if (!wait_user_scan_done(seq, grace)) {
      scan_end_if(seq); /* a SCAN_DONE after this is ignored */
    }
    esp_wifi_clear_ap_list();
    err = ESP_ERR_TIMEOUT;
    goto out;
  }
  taskENTER_CRITICAL(&s.lock);
  status = s.user_scan_status;
  taskEXIT_CRITICAL(&s.lock);
  if (status != 0) {
    ESP_LOGW(TAG, "scan: failed (status %u)", (unsigned)status);
    esp_wifi_clear_ap_list();
    err = ESP_FAIL;
    goto out;
  }

  err = take_scan_records(&recs, &n);
  if (err != ESP_OK || n == 0) {
    goto out;
  }
  views = alloc_psram((size_t)n * sizeof(*views));
  if (views == NULL) {
    err = ESP_ERR_NO_MEM;
    goto out;
  }
  to_views(recs, n, views);
  count = wl_shape_scan(views, n, idx, WL_SCAN_LIST_MAX);
  if (count > 0) {
    wifi_ap_record_t *list = alloc_psram(count * sizeof(*list));
    if (list == NULL) {
      err = ESP_ERR_NO_MEM;
      goto out;
    }
    for (size_t i = 0; i < count; i++) {
      list[i] = recs[idx[i]];
    }
    *ap_list = list;
    *ap_count = (uint16_t)count;
  }
  ESP_LOGI(TAG, "scan: %u networks", (unsigned)count);

out:
  free(views);
  free(recs);
  set_owner(OWNER_NONE);
  xSemaphoreGive(s.op_sem);
  /* A retry that came due while we scanned can go now. */
  if (esp_event_post(WIFI_MODULE_EVENT, EV_RESUME, NULL, 0,
                     pdMS_TO_TICKS(RESUME_POST_TIMEOUT_MS)) != ESP_OK) {
    ESP_LOGW(TAG, "scan: resume event lost; station retries in %u ms",
             (unsigned)WL_BUSY_POLL_MS);
  }
  return err;
}

void wifi_sanitize_hostname(const char *name, char *out, size_t out_len) {
  if (out == NULL || out_len == 0) {
    return;
  }
  uint8_t mac[6];
  read_sta_mac(mac);
  wl_sanitize_hostname(name, mac, out, out_len);
}

void wifi_set_hostname(const char *device_name) {
  if (!atomic_load(&s.started)) {
    return;
  }
  apply_hostname(device_name);
}
