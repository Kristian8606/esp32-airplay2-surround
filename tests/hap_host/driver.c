/* Host driver for the HAP module: one command per stdin line, one JSON line
 * back. tests/hap_host/test_hap.py speaks to it as an independent iOS-style
 * client. Not part of the firmware. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hap.h"
#include "nvs.h"
#include "sodium.h"
#include "srp.h"

/* In-memory NVS, reset by the process. A dump file keeps it across runs. */
static struct { char key[32]; uint8_t val[128]; size_t len; } store[8];
static int stored;
static char last_ns[32];
esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h) { (void)m; snprintf(last_ns, sizeof(last_ns), "%s", ns); *h = 1; return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *o, size_t *l) {
  (void)h;
  for (int i = 0; i < stored; i++) if (!strcmp(store[i].key, k)) {
    if (o && *l < store[i].len) return ESP_ERR_INVALID_SIZE;
    if (o) memcpy(o, store[i].val, store[i].len);
    *l = store[i].len; return ESP_OK;
  }
  return ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t l) {
  (void)h;
  int i; for (i = 0; i < stored; i++) if (!strcmp(store[i].key, k)) break;
  if (i == stored) { if (stored == 8 || l > 128) return ESP_FAIL; stored++; }
  snprintf(store[i].key, sizeof(store[i].key), "%s", k); memcpy(store[i].val, v, l); store[i].len = l; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }

static void hex(const char *name, const uint8_t *b, size_t n, int comma) {
  printf("\"%s\":\"", name);
  if (b) for (size_t i = 0; i < n; i++) printf("%02x", b[i]);
  printf("\"%s", comma ? "," : "");
}
static size_t unhex(const char *s, uint8_t *o, size_t cap) {
  size_t n = 0; unsigned v;
  while (s[0] && s[1] && n < cap && sscanf(s, "%2x", &v) == 1) { o[n++] = (uint8_t)v; s += 2; }
  return n;
}
static hap_session_t *sess;
static uint8_t in[8192], out[4096];

static void state(void) {
  size_t klen = 0; const uint8_t *k = sess && sess->srp ? srp_get_session_key(sess->srp, &klen) : NULL;
  uint8_t audio[32] = {0}; esp_err_t ae = sess ? hap_derive_audio_key(sess, audio, 32) : ESP_FAIL;
  printf("\"pv\":%d,\"ps\":%d,\"transient\":%d,\"established\":%d,\"encn\":%llu,\"decn\":%llu,\"audio_err\":%d,",
         sess->pair_verify_state, sess->pair_setup_state, sess->pair_setup_transient, sess->session_established,
         (unsigned long long)sess->encrypt_nonce, (unsigned long long)sess->decrypt_nonce, ae);
  hex("shared", sess->shared_secret, 32, 1); hex("enc", sess->encrypt_key, 32, 1); hex("dec", sess->decrypt_key, 32, 1);
  hex("k", k, klen, 1); hex("audio", audio, 32, 0);
}
typedef esp_err_t (*step_fn)(hap_session_t *, const uint8_t *, size_t, uint8_t *, size_t, size_t *);
static const struct { const char *n; step_fn f; size_t cap; } steps[] = {
  {"setup1", hap_pair_setup_m1, 2048}, {"setup3", hap_pair_setup_m3, 2048}, {"setup5", hap_pair_setup_m5, 2048},
  {"verify1", hap_pair_verify_m1, 1024}, {"verify3", hap_pair_verify_m3, 1024},
  {"raw1", hap_pair_verify_m1_raw, 1024}, {"raw3", hap_pair_verify_m3_raw, 1024},
};
int main(void) {
  static char line[20000]; setvbuf(stdout, NULL, _IOLBF, 0);
  while (fgets(line, sizeof(line), stdin)) {
    char cmd[16] = {0}, *arg = strchr(line, ' '); line[strcspn(line, "\r\n")] = 0;
    sscanf(line, "%15s", cmd); printf("{");
    if (!strcmp(cmd, "init")) { esp_err_t e = hap_init(); printf("\"err\":%d,\"ns\":\"%s\",", e, last_ns); hex("pk", hap_get_public_key(), 32, 0); }
    else if (!strcmp(cmd, "new")) { if (sess) hap_session_free(sess); sess = hap_session_create(); printf("\"ok\":%d", sess != NULL); }
    else if (!strcmp(cmd, "state")) state();
    else if (!strcmp(cmd, "nvs")) { /* what hap_init stored */
      printf("\"ns\":\"%s\"", last_ns);
      for (int i = 0; i < stored; i++) { printf(","); hex(store[i].key, store[i].val, store[i].len, 0); }
    }
    else if (!strcmp(cmd, "nvsset")) { /* key hex: an identity from another build */
      char key[32]; static char val[300]; uint8_t raw[128];
      if (sscanf(line, "%*s %31s %299s", key, val) == 2) { size_t n = unhex(val, raw, sizeof(raw)); nvs_set_blob(1, key, raw, n); }
      printf("\"ok\":1");
    }
    else {
      int done = 0;
      for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) if (!strcmp(cmd, steps[i].n)) {
        size_t n = arg ? unhex(arg + 1, in, sizeof(in)) : 0, ol = 0;
        esp_err_t e = steps[i].f(sess, in, n, out, steps[i].cap, &ol);
        printf("\"err\":%d,", e); hex("out", out, ol, 1); state(); done = 1; break;
      }
      if (!done) printf("\"bad\":\"%s\"", cmd);
    }
    printf("}\n");
  }
  return 0;
}
