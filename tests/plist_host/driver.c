/* Host driver for the plist module: one command per stdin line, one JSON line
 * back. tests/plist_host/test_plist.py compares it with an independent plist
 * implementation. Not part of the firmware. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plist.h"

static void hex(const char *n, const uint8_t *b, size_t l) { printf("\"%s\":\"", n); for (size_t i = 0; i < l; i++) printf("%02x", b[i]); printf("\""); }
static size_t unhex(const char *s, uint8_t *o, size_t cap) { size_t n = 0; unsigned v; while (s[0] && s[1] && n < cap && sscanf(s, "%2x", &v) == 1) { o[n++] = (uint8_t)v; s += 2; } return n; }
static uint8_t body[1 << 20], outb[1 << 16];
static char line[1 << 22], a[4096], c[1 << 21];
static void jstr(const char *s) { putchar('"'); for (; *s; s++) { unsigned char u = (unsigned char)*s; if (u < 0x20 || u == '"' || u == '\\' || u >= 0x7f) printf("\\u%04x", u); else putchar(u); } putchar('"'); }

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  while (fgets(line, sizeof(line), stdin)) {
    char cmd[32] = {0}; long long n1 = 0; double d = 0; size_t blen = 0;
    line[strcspn(line, "\r\n")] = 0;
    a[0] = 0; c[0] = 0;
    /* cmd key number hexbody ('-' = empty) */
    sscanf(line, "%31s %4095s %lld %2097151s", cmd, a, &n1, c);
    if (c[0] != '-') blen = unhex(c, body, sizeof(body));
    printf("{");
    if (!strcmp(cmd, "data")) {
      size_t ol = (size_t)-1; bool r = bplist_find_data_deep(body, blen, a, outb, (size_t)n1, &ol);
      printf("\"r\":%d,", r); hex("v", outb, r ? ol : 0);
    } else if (!strcmp(cmd, "int")) {
      int64_t v = 0; bool r = bplist_find_int(body, blen, a, &v); printf("\"r\":%d,\"v\":%" PRId64, r, r ? v : 0);
    } else if (!strcmp(cmd, "real")) {
      double v = 0; bool r = bplist_find_real(body, blen, a, &v); printf("\"r\":%d,\"v\":%.17g", r, r ? v : 0);
    } else if (!strcmp(cmd, "str")) {
      static char s[4096]; memset(s, 0, sizeof(s)); bool r = bplist_find_string(body, blen, a, s, (size_t)n1); printf("\"r\":%d,\"v\":", r); jstr(r ? s : "");
    } else if (!strcmp(cmd, "count")) {
      size_t v = 0; bool r = bplist_get_streams_count(body, blen, &v); printf("\"r\":%d,\"v\":%zu", r, r ? v : 0);
    } else if (!strcmp(cmd, "info")) {
      int64_t t = -77; size_t e = 777, i = 777, s = 777; bool r = bplist_get_stream_info(body, blen, (size_t)n1, &t, &e, &i, &s);
      printf("\"r\":%d,\"t\":%" PRId64 ",\"e\":%zu,\"i\":%zu,\"s\":%zu", r, t, e, i, s);
    } else if (!strcmp(cmd, "kv")) {
      static bplist_kv_info_t kv[32]; size_t cnt = 0; size_t cap = (size_t)atoi(a); memset(kv, 0, sizeof(kv));
      bool r = bplist_get_stream_kv_info(body, blen, (size_t)n1, kv, cap, &cnt);
      printf("\"r\":%d,\"n\":%zu,\"kv\":[", r, cnt);
      for (size_t i = 0; r && i < cnt && i < 32; i++) { printf("%s[", i ? "," : ""); jstr(kv[i].key); printf(",%u,%zu,%" PRId64 "]", kv[i].value_type, kv[i].value_len, kv[i].int_value); }
      printf("]");
    } else if (!strcmp(cmd, "crypto")) {
      uint8_t e[256], i[64], s[64]; size_t el = 999, il = 999, sl = 999; int ce = 256, ci = 64, cs = 64; sscanf(a, "%d,%d,%d", &ce, &ci, &cs);
      bool r = bplist_find_stream_crypto(body, blen, n1, ce ? e : NULL, (size_t)ce, &el, ci ? i : NULL, (size_t)ci, &il, cs ? s : NULL, (size_t)cs, &sl);
      printf("\"r\":%d,\"el\":%zu,\"il\":%zu,\"sl\":%zu,", r, el, il, sl);
      hex("e", e, r && el < 257 ? el : 0); printf(","); hex("i", i, r && il < 65 ? il : 0); printf(","); hex("s", s, r && sl < 65 ? sl : 0);
    } else if (!strcmp(cmd, "binit")) { size_t l = bplist_build_initial_setup(outb, (size_t)atoi(a), (uint16_t)n1); hex("v", outb, l);
    } else if (!strcmp(cmd, "bstream")) { unsigned t, dp, cp, ab; sscanf(a, "%u,%u,%u,%u", &t, &dp, &cp, &ab); size_t l = bplist_build_stream_setup(outb, (size_t)n1, t, (uint16_t)dp, (uint16_t)cp, ab); hex("v", outb, l);
    } else if (!strcmp(cmd, "bfeed")) { unsigned t; sscanf(a, "%u,%lf", &t, &d); size_t l = bplist_build_feedback_response(outb, (size_t)n1, t, d); hex("v", outb, l);
    } else if (!strcmp(cmd, "binfo")) {
      static const uint8_t pk[32] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32};
      char id[64], name[256]; unsigned long long f; sscanf(a, "%63[^,],%255[^,],%llu,%lf", id, name, &f, &d);
      size_t l = bplist_build_info_response(outb, (size_t)n1, id, name, pk, 32, f, d); hex("v", outb, l);
    } else if (!strcmp(cmd, "bcmd")) { size_t l = bplist_build_media_remote_command(outb, (size_t)atoi(a), (uint8_t)n1); hex("v", outb, l);
    } else if (!strcmp(cmd, "bvol")) { sscanf(a, "%lf", &d); size_t l = bplist_build_media_remote_volume(outb, (size_t)n1, d); hex("v", outb, l);
    } else if (!strcmp(cmd, "xml")) {
      /* Every builder call, as /info uses them. n1 = capacity */
      static char x[1 << 16]; plist_t p; size_t cap = (size_t)n1; static const uint8_t dat[5] = {1, 2, 3, 250, 251};
      memset(x, 'Z', sizeof(x));
      plist_init(&p, x, cap); plist_begin(&p); plist_dict_begin(&p);
      plist_dict_string(&p, "name", "A & B <c> \"q\" 'r'"); plist_dict_int(&p, "neg", -5); plist_dict_uint(&p, "big", 18446744073709551615ULL);
      plist_dict_data(&p, "pk", dat, 5); plist_dict_data(&p, "none", dat, 0);
      plist_dict_array_begin(&p, "arr"); plist_dict_begin(&p); plist_dict_int(&p, "x", 1); plist_dict_end(&p); plist_array_end(&p);
      plist_dict_dict_begin(&p, "sub"); plist_dict_string(&p, "y", "z"); plist_dict_end(&p);
      plist_dict_end(&p); size_t l = plist_end(&p);
      int over = 0; for (size_t i = cap; i < sizeof(x); i++) if (x[i] != 'Z') over = 1;
      printf("\"len\":%zu,\"term\":%d,\"over\":%d,", l, cap ? x[l < cap ? l : cap - 1] == 0 : -1, over); hex("v", (uint8_t *)x, l < cap ? l : (cap ? cap - 1 : 0));
    } else if (!strcmp(cmd, "b64")) {
      static char o[8192]; size_t il = unhex(a[0] == '-' ? "" : a, body, 4000); memset(o, 'Z', sizeof(o));
      int r = base64_encode(body, il, o, (size_t)n1); size_t need = base64_encoded_length(il);
      printf("\"r\":%d,\"need\":%zu,\"v\":", r, need); o[sizeof(o) - 1] = 0; if (r >= 0) { size_t k = strnlen(o, sizeof(o) - 1); if (k > need) o[need] = 0; jstr(o); } else printf("\"\"");
    } else printf("\"bad\":1");
    printf("}\n");
  }
  return 0;
}
