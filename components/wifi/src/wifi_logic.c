/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wifi_logic.h"

#include <stdio.h>
#include <string.h>

size_t wl_ssid_len(const void *ssid) {
  if (ssid == NULL) {
    return 0;
  }
  const uint8_t *p = ssid;
  size_t n = 0;
  while (n < WL_SSID_MAX_LEN && p[n] != 0) {
    n++;
  }
  return n;
}

bool wl_ssid_equal(const void *a, const void *b) {
  size_t la = wl_ssid_len(a);
  return la == wl_ssid_len(b) && (la == 0 || memcmp(a, b, la) == 0);
}

bool wl_sec_allowed(bool has_password, wl_sec_t sec) {
  return has_password ? sec == WL_SEC_PSK : sec == WL_SEC_OPEN;
}

static bool is_alnum(unsigned char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
         (c >= 'a' && c <= 'z');
}

void wl_sanitize_hostname(const char *name, const uint8_t mac[6], char *out,
                          size_t out_len) {
  if (out == NULL || out_len == 0) {
    return;
  }
  size_t max = out_len - 1;
  if (max > WL_HOSTNAME_MAX_LEN) {
    max = WL_HOSTNAME_MAX_LEN;
  }

  /* A run of other bytes becomes one '-', written only when a letter or
   * digit follows it and fits after it: so no '-' at the start or the end,
   * also after the cut. */
  size_t n = 0;
  if (name != NULL) {
    bool gap = false;
    for (const unsigned char *p = (const unsigned char *)name; *p != 0; p++) {
      if (!is_alnum(*p)) {
        gap = true;
        continue;
      }
      if (gap && n > 0) {
        if (n + 1 >= max) {
          break;
        }
        out[n++] = '-';
      }
      gap = false;
      if (n >= max) {
        break;
      }
      out[n++] = (char)*p;
    }
  }

  if (n == 0) {
    static const uint8_t zero_mac[6] = {0};
    const uint8_t *m = mac != NULL ? mac : zero_mac;
    char def[sizeof(WL_DEFAULT_HOSTNAME_PREFIX) + 6];
    snprintf(def, sizeof(def), WL_DEFAULT_HOSTNAME_PREFIX "%02x%02x%02x", m[3],
             m[4], m[5]);
    n = strlen(def);
    if (n > max) {
      n = max;
    }
    memcpy(out, def, n);
    while (n > 0 && out[n - 1] == '-') {
      n--;
    }
  }
  out[n] = '\0';
}

void wl_mac_str(const uint8_t mac[6], char *out, size_t len) {
  if (out == NULL || len == 0) {
    return;
  }
  if (len < 18 || mac == NULL) {
    out[0] = '\0';
    return;
  }
  snprintf(out, len, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);
}

esp_err_t wl_ip_str(bool connected, uint32_t ip, char *out, size_t len) {
  if (out == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  if (len < 16) {
    if (len > 0) {
      out[0] = '\0';
    }
    return ESP_ERR_INVALID_SIZE;
  }
  if (!connected) {
    out[0] = '\0';
    return ESP_ERR_INVALID_STATE;
  }
  uint8_t b[4];
  memcpy(b, &ip, sizeof(b));
  snprintf(out, len, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
  return ESP_OK;
}

uint32_t wl_ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  const uint8_t bytes[4] = {a, b, c, d};
  uint32_t ip;
  memcpy(&ip, bytes, sizeof(ip));
  return ip;
}

void wl_ap_params(const char *ssid, const char *password, wl_ap_params_t *out) {
  memset(out, 0, sizeof(*out));

  size_t ssid_len = ssid != NULL ? strnlen(ssid, WL_SSID_MAX_LEN + 1) : 0;
  if (ssid_len >= 1 && ssid_len <= WL_SSID_MAX_LEN) {
    memcpy(out->ssid, ssid, ssid_len);
  } else {
    out->bad_ssid = ssid != NULL;
    ssid_len = strlen(WL_DEFAULT_AP_SSID);
    memcpy(out->ssid, WL_DEFAULT_AP_SSID, ssid_len);
  }
  out->ssid_len = (uint8_t)ssid_len;

  if (password == NULL) {
    return;
  }
  size_t pw_len = strnlen(password, WL_PASSWORD_MAX_LEN);
  if (pw_len >= 8 && pw_len <= 63) {
    memcpy(out->password, password, pw_len);
    out->password_len = (uint8_t)pw_len;
    out->wpa2 = true;
  } else {
    out->bad_password = true;
  }
}

int wl_choose(const wl_ap_t *aps, size_t ap_count, const wl_saved_t *saved,
              size_t saved_count, size_t *saved_index) {
  int best = -1;
  size_t best_saved = 0;
  if (aps == NULL || saved == NULL) {
    return -1;
  }
  for (size_t i = 0; i < ap_count && i <= INT32_MAX; i++) {
    if (wl_ssid_len(aps[i].ssid) == 0) {
      continue;
    }
    if (best >= 0 && aps[i].rssi <= aps[best].rssi) {
      continue;
    }
    for (size_t j = 0; j < saved_count; j++) {
      if (wl_ssid_equal(aps[i].ssid, saved[j].ssid) &&
          wl_sec_allowed(saved[j].has_password, aps[i].sec)) {
        best = (int)i;
        best_saved = j;
        break;
      }
    }
  }
  if (best >= 0 && saved_index != NULL) {
    *saved_index = best_saved;
  }
  return best;
}

size_t wl_shape_scan(const wl_ap_t *aps, size_t ap_count, uint16_t *out,
                     size_t out_max) {
  if (aps == NULL || out == NULL) {
    return 0;
  }
  if (ap_count > UINT16_MAX) {
    ap_count = UINT16_MAX;
  }
  /* Selection: each round takes the strongest access point whose SSID is
   * not in the list yet. At most out_max (32) rounds, no extra memory. */
  size_t n = 0;
  while (n < out_max) {
    long best = -1;
    for (size_t i = 0; i < ap_count; i++) {
      if (wl_ssid_len(aps[i].ssid) == 0) {
        continue;
      }
      if (best >= 0 && aps[i].rssi <= aps[best].rssi) {
        continue;
      }
      bool taken = false;
      for (size_t k = 0; k < n && !taken; k++) {
        taken = wl_ssid_equal(aps[out[k]].ssid, aps[i].ssid);
      }
      if (!taken) {
        best = (long)i;
      }
    }
    if (best < 0) {
      break;
    }
    out[n++] = (uint16_t)best;
  }
  return n;
}

wl_retry_t wl_retry_after(unsigned failures, bool ap_has_clients) {
  static const uint32_t same_ms[] = {0, 1000, 2000, 4000, 8000, 16000};
  const unsigned same_count = sizeof(same_ms) / sizeof(same_ms[0]);
  if (failures == 0) {
    failures = 1;
  }
  if (failures <= same_count) {
    return (wl_retry_t){WL_RETRY_SAME, same_ms[failures - 1]};
  }
  return (wl_retry_t){WL_RETRY_FULL, ap_has_clients ? 60000 : 30000};
}

const char *wl_reason_name(unsigned reason) {
  static const struct {
    uint8_t code;
    const char *name;
  } names[] = {
      {1, "UNSPECIFIED"},
      {2, "AUTH_EXPIRE"},
      {3, "AUTH_LEAVE"},
      {4, "DISASSOC_DUE_TO_INACTIVITY"},
      {5, "ASSOC_TOOMANY"},
      {6, "CLASS2_FRAME_FROM_NONAUTH_STA"},
      {7, "CLASS3_FRAME_FROM_NONASSOC_STA"},
      {8, "ASSOC_LEAVE"},
      {9, "ASSOC_NOT_AUTHED"},
      {10, "DISASSOC_PWRCAP_BAD"},
      {11, "DISASSOC_SUPCHAN_BAD"},
      {12, "BSS_TRANSITION_DISASSOC"},
      {13, "IE_INVALID"},
      {14, "MIC_FAILURE"},
      {15, "4WAY_HANDSHAKE_TIMEOUT"},
      {16, "GROUP_KEY_UPDATE_TIMEOUT"},
      {17, "IE_IN_4WAY_DIFFERS"},
      {18, "GROUP_CIPHER_INVALID"},
      {19, "PAIRWISE_CIPHER_INVALID"},
      {20, "AKMP_INVALID"},
      {21, "UNSUPP_RSN_IE_VERSION"},
      {22, "INVALID_RSN_IE_CAP"},
      {23, "802_1X_AUTH_FAILED"},
      {24, "CIPHER_SUITE_REJECTED"},
      {25, "TDLS_PEER_UNREACHABLE"},
      {26, "TDLS_UNSPECIFIED"},
      {27, "SSP_REQUESTED_DISASSOC"},
      {28, "NO_SSP_ROAMING_AGREEMENT"},
      {29, "BAD_CIPHER_OR_AKM"},
      {30, "NOT_AUTHORIZED_THIS_LOCATION"},
      {31, "SERVICE_CHANGE_PRECLUDES_TS"},
      {32, "UNSPECIFIED_QOS"},
      {33, "NOT_ENOUGH_BANDWIDTH"},
      {34, "MISSING_ACKS"},
      {35, "EXCEEDED_TXOP"},
      {36, "STA_LEAVING"},
      {37, "END_BA"},
      {38, "UNKNOWN_BA"},
      {39, "TIMEOUT"},
      {46, "PEER_INITIATED"},
      {47, "AP_INITIATED"},
      {48, "INVALID_FT_ACTION_FRAME_COUNT"},
      {49, "INVALID_PMKID"},
      {50, "INVALID_MDE"},
      {51, "INVALID_FTE"},
      {67, "TX_LINK_EST_FAILED"},
      {68, "ALTERATIVE_CHANNEL_OCCUPIED"},
      {200, "BEACON_TIMEOUT"},
      {201, "NO_AP_FOUND"},
      {202, "AUTH_FAIL"},
      {203, "ASSOC_FAIL"},
      {204, "HANDSHAKE_TIMEOUT"},
      {205, "CONNECTION_FAIL"},
      {206, "AP_TSF_RESET"},
      {207, "ROAMING"},
      {208, "ASSOC_COMEBACK_TIME_TOO_LONG"},
      {209, "SA_QUERY_TIMEOUT"},
      {210, "NO_AP_FOUND_SECURITY"},
      {211, "NO_AP_FOUND_AUTHMODE"},
      {212, "NO_AP_FOUND_RSSI"},
  };
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    if (names[i].code == reason) {
      return names[i].name;
    }
  }
  return "UNKNOWN";
}
