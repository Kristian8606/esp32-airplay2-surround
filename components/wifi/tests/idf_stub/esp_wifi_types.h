/*
 * SPDX-License-Identifier: Apache-2.0
 */
/* Stand-in for the ESP-IDF header of the same name: see README.md. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
  WIFI_MODE_NULL,
  WIFI_MODE_STA,
  WIFI_MODE_AP,
  WIFI_MODE_APSTA,
} wifi_mode_t;
typedef enum { WIFI_IF_STA, WIFI_IF_AP } wifi_interface_t;
typedef enum {
  WIFI_AUTH_OPEN,
  WIFI_AUTH_WEP,
  WIFI_AUTH_WPA_PSK,
  WIFI_AUTH_WPA2_PSK,
  WIFI_AUTH_WPA_WPA2_PSK,
  WIFI_AUTH_ENTERPRISE,
  WIFI_AUTH_WPA2_ENTERPRISE = WIFI_AUTH_ENTERPRISE,
  WIFI_AUTH_WPA3_PSK,
  WIFI_AUTH_WPA2_WPA3_PSK,
  WIFI_AUTH_WAPI_PSK,
  WIFI_AUTH_OWE,
  WIFI_AUTH_WPA3_ENT_192,
  WIFI_AUTH_DUMMY_1,
  WIFI_AUTH_DUMMY_2,
  WIFI_AUTH_DPP,
  WIFI_AUTH_WPA3_ENTERPRISE,
  WIFI_AUTH_WPA2_WPA3_ENTERPRISE,
  WIFI_AUTH_WPA_ENTERPRISE,
  WIFI_AUTH_MAX
} wifi_auth_mode_t;
typedef enum { WIFI_CIPHER_TYPE_NONE } wifi_cipher_type_t;
typedef enum { WIFI_SECOND_CHAN_NONE } wifi_second_chan_t;
typedef enum { WIFI_BW_HT20 = 1 } wifi_bandwidth_t;
typedef enum { WIFI_ANT_ANT0 } wifi_ant_t;
typedef enum { WIFI_SCAN_TYPE_ACTIVE, WIFI_SCAN_TYPE_PASSIVE } wifi_scan_type_t;
typedef enum { WIFI_FAST_SCAN, WIFI_ALL_CHANNEL_SCAN } wifi_scan_method_t;
typedef enum {
  WIFI_CONNECT_AP_BY_SIGNAL,
  WIFI_CONNECT_AP_BY_SECURITY,
} wifi_sort_method_t;
typedef enum {
  WPA3_SAE_PWE_UNSPECIFIED,
  WPA3_SAE_PWE_HUNT_AND_PECK,
  WPA3_SAE_PWE_HASH_TO_ELEMENT,
  WPA3_SAE_PWE_BOTH,
} wifi_sae_pwe_method_t;
typedef enum { WPA3_SAE_PK_MODE_AUTOMATIC } wifi_sae_pk_mode_t;
typedef enum { WIFI_PS_NONE, WIFI_PS_MIN_MODEM, WIFI_PS_MAX_MODEM } wifi_ps_type_t;
typedef enum { WIFI_STORAGE_FLASH, WIFI_STORAGE_RAM } wifi_storage_t;

typedef struct {
  char cc[3];
  uint8_t schan;
  uint8_t nchan;
  int8_t max_tx_power;
  int policy;
} wifi_country_t;
typedef struct {
  uint8_t bss_color : 6;
  uint8_t partial_bss_color : 1;
  uint8_t bss_color_disabled : 1;
  uint8_t bssid_index;
} wifi_he_ap_info_t;
typedef struct {
  uint8_t bssid[6];
  uint8_t ssid[33];
  uint8_t primary;
  wifi_second_chan_t second;
  int8_t rssi;
  wifi_auth_mode_t authmode;
  wifi_cipher_type_t pairwise_cipher;
  wifi_cipher_type_t group_cipher;
  wifi_ant_t ant;
  uint32_t phy_11b : 1, phy_11g : 1, phy_11n : 1, phy_lr : 1, phy_11a : 1,
      phy_11ac : 1, phy_11ax : 1, wps : 1, ftm_responder : 1,
      ftm_initiator : 1, reserved : 22;
  wifi_country_t country;
  wifi_he_ap_info_t he_ap;
  wifi_bandwidth_t bandwidth;
  uint8_t vht_ch_freq1;
  uint8_t vht_ch_freq2;
} wifi_ap_record_t;

typedef struct {
  uint32_t min;
  uint32_t max;
} wifi_active_scan_time_t;
typedef struct {
  wifi_active_scan_time_t active;
  uint32_t passive;
} wifi_scan_time_t;
typedef struct {
  uint16_t ghz_2_channels;
  uint32_t ghz_5_channels;
} wifi_scan_channel_bitmap_t;
typedef struct {
  uint8_t *ssid;
  uint8_t *bssid;
  uint8_t channel;
  bool show_hidden;
  wifi_scan_type_t scan_type;
  wifi_scan_time_t scan_time;
  uint8_t home_chan_dwell_time;
  wifi_scan_channel_bitmap_t channel_bitmap;
  bool coex_background_scan;
} wifi_scan_config_t;

typedef struct {
  int8_t rssi;
  wifi_auth_mode_t authmode;
  uint8_t rssi_5g_adjustment;
} wifi_scan_threshold_t;
typedef struct {
  bool capable;
  bool required;
} wifi_pmf_config_t;
typedef struct {
  uint8_t ssid[32];
  uint8_t password[64];
  wifi_scan_method_t scan_method;
  bool bssid_set;
  uint8_t bssid[6];
  uint8_t channel;
  uint16_t listen_interval;
  wifi_sort_method_t sort_method;
  wifi_scan_threshold_t threshold;
  wifi_pmf_config_t pmf_cfg;
  uint32_t rm_enabled : 1, btm_enabled : 1, mbo_enabled : 1, ft_enabled : 1,
      owe_enabled : 1, transition_disable : 1,
      disable_wpa3_compatible_mode : 1, reserved1 : 25;
  wifi_sae_pwe_method_t sae_pwe_h2e;
  wifi_sae_pk_mode_t sae_pk_mode;
  uint8_t failure_retry_cnt;
  uint32_t he_dcm_set : 1, he_dcm_max_constellation_tx : 2,
      he_dcm_max_constellation_rx : 2, he_mcs9_enabled : 1,
      he_su_beamformee_disabled : 1, he_trig_su_bmforming_feedback_disabled : 1,
      he_trig_mu_bmforming_partial_feedback_disabled : 1,
      he_trig_cqi_feedback_disabled : 1, vht_su_beamformee_disabled : 1,
      vht_mu_beamformee_disabled : 1, vht_mcs8_enabled : 1, reserved2 : 19;
  uint8_t sae_h2e_identifier[32];
} wifi_sta_config_t;
typedef struct {
  bool protected_keep_alive;
  uint16_t period;
} wifi_bss_max_idle_config_t;
typedef struct {
  uint8_t ssid[32];
  uint8_t password[64];
  uint8_t ssid_len;
  uint8_t channel;
  wifi_auth_mode_t authmode;
  uint8_t ssid_hidden;
  uint8_t max_connection;
  uint16_t beacon_interval;
  uint8_t csa_count;
  uint8_t dtim_period;
  wifi_cipher_type_t pairwise_cipher;
  bool ftm_responder;
  wifi_pmf_config_t pmf_cfg;
  wifi_sae_pwe_method_t sae_pwe_h2e;
  uint8_t transition_disable;
  uint8_t sae_ext;
  uint8_t wpa3_compatible_mode;
  uint8_t reserved;
  wifi_bss_max_idle_config_t bss_max_idle_cfg;
  uint16_t gtk_rekey_interval;
} wifi_ap_config_t;
typedef union {
  wifi_ap_config_t ap;
  wifi_sta_config_t sta;
} wifi_config_t;

typedef enum {
  WIFI_EVENT_WIFI_READY,
  WIFI_EVENT_SCAN_DONE,
  WIFI_EVENT_STA_START,
  WIFI_EVENT_STA_STOP,
  WIFI_EVENT_STA_CONNECTED,
  WIFI_EVENT_STA_DISCONNECTED,
  WIFI_EVENT_AP_START = 12,
  WIFI_EVENT_AP_STOP,
  WIFI_EVENT_AP_STACONNECTED,
  WIFI_EVENT_AP_STADISCONNECTED,
} wifi_event_t;
typedef struct {
  uint32_t status;
  uint8_t number;
  uint8_t scan_id;
} wifi_event_sta_scan_done_t;
typedef struct {
  uint8_t ssid[32];
  uint8_t ssid_len;
  uint8_t bssid[6];
  uint8_t channel;
  wifi_auth_mode_t authmode;
  uint16_t aid;
} wifi_event_sta_connected_t;
typedef struct {
  uint8_t ssid[32];
  uint8_t ssid_len;
  uint8_t bssid[6];
  uint8_t reason;
  int8_t rssi;
} wifi_event_sta_disconnected_t;
