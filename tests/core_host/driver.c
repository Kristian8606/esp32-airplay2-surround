/* Host driver for the small network and crypto helpers: one command per stdin
 * line, one JSON line back. Not part of the firmware. */
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "ap2_audio_crypto.h"
#include "audio_receiver.h"
#include "mdns.h"
#include "mdns_airplay.h"
#include "rtsp_events.h"
#include "settings.h"
#include "socket_utils.h"
#include "wifi.h"

static const char *s_name = "Living Room";
void wifi_sanitize_hostname(const char *in, char *out, size_t len) { snprintf(out, len, "h-%s", in); for (char *p = out; *p; p++) if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) *p = '-'; }
void wifi_get_mac_str(char *m, size_t l) { snprintf(m, l, "AA:BB:CC:01:02:03"); }
esp_err_t settings_get_device_name(char *n, size_t l) { if (!s_name) return ESP_FAIL; snprintf(n, l, "%s", s_name); return ESP_OK; }
const uint8_t *ap2_hap_identity_public_key(void) { static uint8_t k[32]; for (int i = 0; i < 32; i++) k[i] = (uint8_t)(i * 7 + 1); return k; }
void airplay_get_pairing_id(char *o, size_t l) { snprintf(o, l, "11111111-2222-4333-8444-555555555555"); }

static void js(const char *s) { putchar('"'); for (; s && *s; s++) { unsigned char c = (unsigned char)*s; if (c < 32 || c == '"' || c == '\\' || c > 126) printf("\\u%04x", c); else putchar(c); } putchar('"'); }
static int s_first = 1;
esp_err_t mdns_init(void) { printf("%s[\"init\"]", s_first ? "" : ","); s_first = 0; return ESP_OK; }
esp_err_t mdns_hostname_set(const char *h) { printf("%s[\"host\",", s_first ? "" : ","); js(h); printf("]"); s_first = 0; return ESP_OK; }
esp_err_t mdns_service_add(const char *in, const char *ty, const char *pr, uint16_t port, mdns_txt_item_t *t, size_t n) {
  printf("%s[\"svc\",", s_first ? "" : ","); js(in); printf(","); js(ty); printf(","); js(pr); printf(",%u,[", port);
  for (size_t i = 0; i < n; i++) { printf("%s[", i ? "," : ""); js(t[i].key); printf(","); js(t[i].value); printf("]"); }
  printf("]]"); s_first = 0; return ESP_OK;
}
static int ev_log[64], ev_n; static void ev_cb(rtsp_event_t e, void *u) { if (ev_n < 64) ev_log[ev_n++] = (int)e * 100 + (int)(long)u; }
#define CB(n) static void cb##n(rtsp_event_t e, void *u) { if (ev_n < 64) ev_log[ev_n++] = (int)e * 100 + 10 * n + (int)(long)u; }
CB(0) CB(1) CB(2) CB(3) CB(4) CB(5) CB(6) CB(7) CB(8) CB(9)
static const rtsp_event_callback_t cbs[10] = {cb0, cb1, cb2, cb3, cb4, cb5, cb6, cb7, cb8, cb9};

static void hex(const uint8_t *b, size_t l) { putchar('"'); for (size_t i = 0; i < l; i++) printf("%02x", b[i]); putchar('"'); }
static size_t unhex(const char *s, uint8_t *o, size_t cap) { size_t n = 0; unsigned v; while (s[0] && s[1] && n < cap && sscanf(s, "%2x", &v) == 1) { o[n++] = (uint8_t)v; s += 2; } return n; }
static char line[1 << 18], a[1 << 17], b[1 << 17]; static uint8_t in[1 << 16], full[1 << 16], out[1 << 16];

static void socket_facts(int fd, unsigned bound) {
  printf("\"fd\":%d,\"bound\":%u", fd < 0 ? -1 : 1, bound);
  if (fd < 0) return;
  struct sockaddr_in addr; socklen_t al = sizeof(addr); getsockname(fd, (struct sockaddr *)&addr, &al);
  int v = 0; socklen_t vl = sizeof(v); struct timeval tv = {0}; socklen_t tl = sizeof(tv);
  getsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &v, &vl); int reuse = v; v = 0; vl = sizeof(v);
  getsockopt(fd, SOL_SOCKET, SO_TYPE, &v, &vl); int type = v; getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, &tl);
  int rb = 0; vl = sizeof(rb); getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rb, &vl); int acc = 0; vl = sizeof(acc); getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &acc, &vl);
  printf(",\"port_ok\":%d,\"any\":%d,\"reuse\":%d,\"type\":%d,\"timeo\":%ld,\"rcvbuf\":%d,\"listening\":%d,\"nonblock\":%d", ntohs(addr.sin_port) == bound, addr.sin_addr.s_addr == htonl(INADDR_ANY), reuse != 0, type, (long)tv.tv_sec, rb, acc, (fcntl(fd, F_GETFL) & O_NONBLOCK) != 0);
}
int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0); static int held = -1;
  while (fgets(line, sizeof(line), stdin)) {
    char cmd[32] = {0}; int n1 = 0, n2 = 0, n3 = 0; line[strcspn(line, "\r\n")] = 0; a[0] = b[0] = 0; sscanf(line, "%31s", cmd); printf("{");
    if (!strcmp(cmd, "mdns")) { static char nm[128]; if (sscanf(line, "%*s %127[^\n]", nm) == 1) s_name = !strcmp(nm, "-") ? NULL : nm; printf("\"calls\":["); s_first = 1; mdns_airplay_init(); printf("]");
    } else if (!strcmp(cmd, "udp")) { unsigned bp = 77; sscanf(line, "%*s %d %d %d", &n1, &n2, &n3); int fd = socket_utils_bind_udp((uint16_t)n1, n2, n3, (uint16_t *)&bp); bp &= 0xffff; socket_facts(fd, bp); if (fd >= 0) close(fd);
    } else if (!strcmp(cmd, "tcp")) { unsigned bp = 77; sscanf(line, "%*s %d %d %d", &n1, &n2, &n3); int fd = socket_utils_bind_tcp_listener((uint16_t)n1, n2, n3 != 0, (uint16_t *)&bp); bp &= 0xffff; socket_facts(fd, bp); if (fd >= 0) close(fd);
    } else if (!strcmp(cmd, "udpnull")) { int fd = socket_utils_bind_udp(0, 0, 0, NULL); printf("\"fd\":%d", fd < 0 ? -1 : 1); if (fd >= 0) close(fd);
    } else if (!strcmp(cmd, "tcpnull")) { int fd = socket_utils_bind_tcp_listener(0, 1, false, NULL); printf("\"fd\":%d", fd < 0 ? -1 : 1); if (fd >= 0) close(fd);
    } else if (!strcmp(cmd, "hold")) { held = socket(AF_INET, SOCK_STREAM, 0); struct sockaddr_in x = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY)}; bind(held, (struct sockaddr *)&x, sizeof(x)); listen(held, 1); socklen_t l = sizeof(x); getsockname(held, (struct sockaddr *)&x, &l); printf("\"port\":%u", ntohs(x.sin_port));
    } else if (!strcmp(cmd, "evreg")) { int cnt = 0, ok = 0; sscanf(line, "%*s %d", &cnt); for (int i = 0; i < cnt; i++) if (rtsp_events_register(ev_cb, (void *)(long)(i % 10)) == 0) ok++; printf("\"ok\":%d", ok);
    } else if (!strcmp(cmd, "evdiff")) { int cnt = 0; sscanf(line, "%*s %d", &cnt); printf("\"r\":["); for (int i = 0; i < cnt; i++) printf("%s%d", i ? "," : "", rtsp_events_register(cbs[i], (void *)(long)(i % 10))); printf("]");
    } else if (!strcmp(cmd, "evnull")) { printf("\"r\":%d", rtsp_events_register(NULL, NULL));
    } else if (!strcmp(cmd, "emit")) { sscanf(line, "%*s %d", &n1); ev_n = 0; rtsp_events_emit((rtsp_event_t)n1); printf("\"log\":["); for (int i = 0; i < ev_n; i++) printf("%s%d", i ? "," : "", ev_log[i]); printf("]");
    } else if (!strcmp(cmd, "rtp") || !strcmp(cmd, "buf")) {
      /* cmd type keyhex cap inhex fullhex */
      static char kh[128]; static audio_encrypt_t e; int ty = 0, cap = 0; memset(&e, 0, sizeof(e)); strcpy(a, "-"); strcpy(b, "-");
      sscanf(line, "%*s %d %127s %d %131071s %131071s", &ty, kh, &cap, a, b); e.type = ty; unhex(kh, e.key, 32); e.key_len = 32;
      size_t il = a[0] == '-' ? 0 : unhex(a, in, sizeof(in)), fl = b[0] == '-' ? 0 : unhex(b, full, sizeof(full)); memset(out, 0xEE, sizeof(out)); int r;
      if (!strcmp(cmd, "rtp")) r = ap2_audio_decrypt_rtp(audio_encrypt_key(&e), in, il, out, (size_t)cap, fl ? full : NULL, fl); else r = ap2_audio_decrypt_buffered(audio_encrypt_key(&e), in, il, out, (size_t)cap);
      printf("\"r\":%d,\"v\":", r); hex(out, r > 0 ? (size_t)r : 0);
    } else printf("\"bad\":1");
    printf("}\n");
  }
  (void)held; return 0;
}
