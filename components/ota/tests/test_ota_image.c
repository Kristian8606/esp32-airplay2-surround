/* SPDX-License-Identifier: Apache-2.0
 *
 * Host tests of src/ota_image.c: the examples of README.md and damaged
 * image starts. Builds without any ESP-IDF header. */
#include "ota_image.h"
#include "test_util.h"

int g_failures;
int g_checks;

static const char GATEWAY[] = "airplay-gateway_v4.5.49";
static const char SPEAKER[] = "airplay-speaker_v4.5.49";
#define PART_SIZE 0x300000u
#define GATEWAY_LEN 1328736u
#define SPEAKER_LEN 1796816u

static uint8_t gateway_head[EXAMPLE_LEN];
static uint8_t speaker_head[EXAMPLE_LEN];
static uint8_t not_firmware[EXAMPLE_LEN];

/* Checks len bytes of buf from an allocation of exactly len bytes, so that
 * AddressSanitizer catches a read past the end. */
static ota_image_result_t check_exact(const uint8_t *buf, size_t len, const char *running,
                                      ota_image_info_t *info)
{
    uint8_t *copy = malloc(len ? len : 1);
    if (len) {
        memcpy(copy, buf, len);
    }
    ota_image_result_t r = ota_image_check_head(copy, len, running, strlen(running), info);
    free(copy);
    return r;
}

static void set_str(uint8_t *head, size_t desc_off, const char *s)
{
    uint8_t *p = head + OTA_IMAGE_APP_DESC_OFFSET + desc_off;
    memset(p, 0, OTA_IMAGE_DESC_STR_LEN);
    memcpy(p, s, strlen(s));
}

static void test_layout_constants(void)
{
    CHECK_EQ(OTA_IMAGE_HEAD_LEN, 288);
    CHECK_EQ(OTA_IMAGE_APP_DESC_OFFSET, 32);
    /* The examples carry the values at the offsets the checks read. */
    CHECK_EQ(gateway_head[0], 0xE9);
    CHECK_EQ(gateway_head[12] | (gateway_head[13] << 8), 9);
    CHECK_EQ(memcmp(gateway_head + 32, "\x32\x54\xcd\xab", 4), 0);
    CHECK(memcmp(gateway_head + 80, GATEWAY, sizeof(GATEWAY)) == 0);
    CHECK(memcmp(gateway_head + 48, "v4.5.49", 8) == 0);
    CHECK(memcmp(speaker_head + 80, SPEAKER, sizeof(SPEAKER)) == 0);
    CHECK_EQ(gateway_head[23], 1); /* hash_appended */
    CHECK_EQ(speaker_head[23], 1);
}

/* Example 1 */
static void test_ex1_gateway_on_gateway(void)
{
    ota_image_info_t info;
    CHECK_EQ(ota_image_check_size(GATEWAY_LEN, PART_SIZE), OTA_IMAGE_OK);
    CHECK_EQ(check_exact(gateway_head, EXAMPLE_LEN, GATEWAY, &info), OTA_IMAGE_OK);
    CHECK(strcmp(info.project, GATEWAY) == 0);
    CHECK(strcmp(info.version, "v4.5.49") == 0);
    CHECK_EQ(check_exact(gateway_head, OTA_IMAGE_HEAD_LEN, GATEWAY, NULL), OTA_IMAGE_OK);
    /* The speaker on a speaker, likewise. */
    CHECK_EQ(ota_image_check_size(SPEAKER_LEN, PART_SIZE), OTA_IMAGE_OK);
    CHECK_EQ(check_exact(speaker_head, EXAMPLE_LEN, SPEAKER, &info), OTA_IMAGE_OK);
    CHECK(strcmp(info.project, SPEAKER) == 0);
}

/* Examples 2 and 3 */
static void test_ex2_ex3_other_kind(void)
{
    ota_image_info_t info;
    CHECK_EQ(check_exact(speaker_head, EXAMPLE_LEN, GATEWAY, &info), OTA_IMAGE_OTHER_KIND);
    CHECK(strcmp(info.project, SPEAKER) == 0);
    CHECK_EQ(check_exact(gateway_head, EXAMPLE_LEN, SPEAKER, NULL), OTA_IMAGE_OTHER_KIND);
    CHECK_EQ(check_exact(gateway_head, EXAMPLE_LEN, "esp-idf-app_v1", NULL), OTA_IMAGE_OTHER_KIND);
}

/* Example 4 */
static void test_ex4_not_firmware(void)
{
    CHECK_EQ(check_exact(not_firmware, EXAMPLE_LEN, GATEWAY, NULL), OTA_IMAGE_INVALID);
    CHECK_EQ(check_exact(not_firmware, OTA_IMAGE_HEAD_LEN, SPEAKER, NULL), OTA_IMAGE_INVALID);
}

/* Example 5 */
static void test_ex5_chip_esp32(void)
{
    uint8_t h[EXAMPLE_LEN];
    memcpy(h, gateway_head, sizeof(h));
    h[12] = 0;
    h[13] = 0;
    CHECK_EQ(check_exact(h, sizeof(h), GATEWAY, NULL), OTA_IMAGE_INVALID);
    /* Other chips: ESP32-S2 (2), ESP32-C3 (5), ESP32-P4 (18), invalid (0xFFFF). */
    const uint16_t ids[] = { 2, 5, 18, 0xFFFF, 0x0900 };
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        h[12] = (uint8_t)ids[i];
        h[13] = (uint8_t)(ids[i] >> 8);
        CHECK_EQ(check_exact(h, sizeof(h), GATEWAY, NULL), OTA_IMAGE_INVALID);
    }
}

/* Examples 6 and 7, and the limits around them */
static void test_ex6_ex7_sizes(void)
{
    CHECK_EQ(ota_image_check_size(0x300001, PART_SIZE), OTA_IMAGE_BAD_SIZE);
    CHECK_EQ(ota_image_check_size(0x300000, PART_SIZE), OTA_IMAGE_OK);
    CHECK_EQ(ota_image_check_size(100, PART_SIZE), OTA_IMAGE_BAD_SIZE);
    CHECK_EQ(ota_image_check_size(287, PART_SIZE), OTA_IMAGE_BAD_SIZE);
    CHECK_EQ(ota_image_check_size(288, PART_SIZE), OTA_IMAGE_OK);
    CHECK_EQ(ota_image_check_size(0, PART_SIZE), OTA_IMAGE_BAD_SIZE);
    CHECK_EQ(ota_image_check_size((size_t)-1, PART_SIZE), OTA_IMAGE_BAD_SIZE);
    CHECK_EQ(ota_image_check_size(288, 100), OTA_IMAGE_BAD_SIZE);
}

/* Example 8 */
static void test_ex8_other_version(void)
{
    uint8_t h[EXAMPLE_LEN];
    ota_image_info_t info;
    memcpy(h, gateway_head, sizeof(h));
    set_str(h, OTA_IMAGE_DESC_PROJECT_OFFSET, "airplay-gateway_v4.5.10");
    set_str(h, OTA_IMAGE_DESC_VERSION_OFFSET, "v4.5.10");
    CHECK_EQ(check_exact(h, sizeof(h), GATEWAY, &info), OTA_IMAGE_OK);
    CHECK(strcmp(info.version, "v4.5.10") == 0);
    /* Newer, same, and no version after the '_' are accepted too. */
    set_str(h, OTA_IMAGE_DESC_PROJECT_OFFSET, "airplay-gateway_v9.0.0");
    CHECK_EQ(check_exact(h, sizeof(h), GATEWAY, NULL), OTA_IMAGE_OK);
    set_str(h, OTA_IMAGE_DESC_PROJECT_OFFSET, "airplay-gateway_");
    CHECK_EQ(check_exact(h, sizeof(h), GATEWAY, NULL), OTA_IMAGE_OK);
}

/* Example 10 */
static void test_ex10_no_hash(void)
{
    uint8_t h[EXAMPLE_LEN];
    memcpy(h, gateway_head, sizeof(h));
    h[23] = 0;
    CHECK_EQ(check_exact(h, sizeof(h), GATEWAY, NULL), OTA_IMAGE_INVALID);
    memcpy(h, speaker_head, sizeof(h));
    h[23] = 0;
    CHECK_EQ(check_exact(h, sizeof(h), SPEAKER, NULL), OTA_IMAGE_INVALID);
    /* Any value other than 1 is refused, not only 0. */
    for (int v = 2; v < 256; v++) {
        h[23] = (uint8_t)v;
        CHECK_EQ(check_exact(h, sizeof(h), SPEAKER, NULL), OTA_IMAGE_INVALID);
    }
    memcpy(h, gateway_head, sizeof(h));
    h[23] = 2;
    CHECK_EQ(check_exact(h, sizeof(h), GATEWAY, NULL), OTA_IMAGE_INVALID);
    h[23] = 1;
    CHECK_EQ(check_exact(h, sizeof(h), GATEWAY, NULL), OTA_IMAGE_OK);
}

/* Example 11 */
static void test_ex11_running_without_kind(void)
{
    CHECK_EQ(check_exact(gateway_head, EXAMPLE_LEN, "airplay-gateway", NULL), OTA_IMAGE_OTHER_KIND);
    CHECK_EQ(check_exact(speaker_head, EXAMPLE_LEN, "airplay-speaker", NULL), OTA_IMAGE_OTHER_KIND);
}

/* Short buffers: every length below the head is refused, never read past. */
static void test_short_buffers(void)
{
    for (size_t len = 0; len < OTA_IMAGE_HEAD_LEN; len++) {
        CHECK_EQ(check_exact(gateway_head, len, GATEWAY, NULL), OTA_IMAGE_BAD_SIZE);
        CHECK_EQ(check_exact(not_firmware, len, GATEWAY, NULL), OTA_IMAGE_BAD_SIZE);
    }
    CHECK_EQ(ota_image_check_head(NULL, 512, GATEWAY, sizeof(GATEWAY), NULL), OTA_IMAGE_BAD_SIZE);
}

/* What changing the byte at offset off should give, from the field map of
 * README: magic, chip id, hash_appended (only 1 is accepted), description
 * magic, and the kind with its '_'. */
static ota_image_result_t expected_for_offset(size_t off, size_t kind_len)
{
    if (off == 0) return OTA_IMAGE_INVALID;
    if (off == 12 || off == 13) return OTA_IMAGE_INVALID;
    if (off == 23) return OTA_IMAGE_INVALID;
    if (off >= 32 && off < 36) return OTA_IMAGE_INVALID;
    size_t name = OTA_IMAGE_APP_DESC_OFFSET + OTA_IMAGE_DESC_PROJECT_OFFSET;
    if (off >= name && off <= name + kind_len) return OTA_IMAGE_OTHER_KIND;
    return OTA_IMAGE_OK;
}

/* Every single byte of the first 288 changed to every other value. */
static void damage_each_byte(const uint8_t *orig, const char *running)
{
    int bad = 0;
    for (size_t off = 0; off < OTA_IMAGE_HEAD_LEN; off++) {
        for (int v = 0; v < 256; v++) {
            if (v == orig[off]) {
                continue;
            }
            ota_image_result_t want = expected_for_offset(off, 15);
            uint8_t h[OTA_IMAGE_HEAD_LEN];
            memcpy(h, orig, sizeof(h));
            h[off] = (uint8_t)v;
            ota_image_info_t info;
            ota_image_result_t got = check_exact(h, sizeof(h), running, &info);
            if (got != want) {
                if (bad++ < 10) {
                    printf("  offset %zu value 0x%02x: got %d want %d\n", off, v, got, want);
                }
            }
            /* The log strings stay terminated and printable. */
            if (got == OTA_IMAGE_OK || got == OTA_IMAGE_OTHER_KIND) {
                CHECK(strlen(info.project) <= OTA_IMAGE_DESC_STR_LEN);
                for (const char *p = info.project; *p; p++) {
                    if (*p < 0x20 || *p >= 0x7F) {
                        bad++;
                    }
                }
            }
        }
    }
    CHECK_EQ(bad, 0);
}

static void test_damaged_headers(void)
{
    damage_each_byte(gateway_head, GATEWAY);
    damage_each_byte(speaker_head, SPEAKER);
}

static void test_kind_edge_cases(void)
{
    CHECK_EQ(ota_image_kind_len(GATEWAY, 32), 15);
    CHECK_EQ(ota_image_kind_len("a_b_c", 32), 1);
    CHECK_EQ(ota_image_kind_len("nounderscore", 32), -1);
    CHECK_EQ(ota_image_kind_len("_v1", 32), -1);
    CHECK_EQ(ota_image_kind_len("", 32), -1);
    CHECK_EQ(ota_image_kind_len(NULL, 32), -1);
    CHECK_EQ(ota_image_kind_len("abc_d", 3), -1); /* '_' past max_len */

    uint8_t h[EXAMPLE_LEN];
    memcpy(h, gateway_head, sizeof(h));
    /* Running firmware without a kind: nothing matches. */
    CHECK_EQ(check_exact(h, sizeof(h), "airplay-gateway", NULL), OTA_IMAGE_OTHER_KIND);
    CHECK_EQ(check_exact(h, sizeof(h), "", NULL), OTA_IMAGE_OTHER_KIND);
    /* A kind that is a prefix of the other is another kind. */
    CHECK_EQ(check_exact(h, sizeof(h), "airplay-gateway2_v1", NULL), OTA_IMAGE_OTHER_KIND);
    CHECK_EQ(check_exact(h, sizeof(h), "airplay_v1", NULL), OTA_IMAGE_OTHER_KIND);
    CHECK_EQ(check_exact(h, sizeof(h), "airplay-gateway_other", NULL), OTA_IMAGE_OK);
    /* Image name without a kind, even equal to the running name. */
    set_str(h, OTA_IMAGE_DESC_PROJECT_OFFSET, "airplay-gateway");
    CHECK_EQ(check_exact(h, sizeof(h), "airplay-gateway", NULL), OTA_IMAGE_OTHER_KIND);
    set_str(h, OTA_IMAGE_DESC_PROJECT_OFFSET, "_v4.5.49");
    CHECK_EQ(check_exact(h, sizeof(h), "_v4.5.49", NULL), OTA_IMAGE_OTHER_KIND);
    /* 32 bytes without a NUL. */
    ota_image_info_t info;
    memcpy(h + 80, "airplay-gateway_ABCDEFGHIJKLMNOP", 32);
    CHECK_EQ(check_exact(h, sizeof(h), GATEWAY, &info), OTA_IMAGE_OK);
    CHECK_EQ(strlen(info.project), 32);
    memcpy(h + 80, "airplay-gatewayXABCDEFGHIJKLMNOP", 32);
    h[112] = '_'; /* a '_' after the 32 bytes does not count */
    CHECK_EQ(check_exact(h, sizeof(h), GATEWAY, NULL), OTA_IMAGE_OTHER_KIND);
    /* Running name given as a 32-byte array without NUL. */
    char run32[32];
    memcpy(run32, "airplay-gateway_ABCDEFGHIJKLMNOP", 32);
    CHECK_EQ(ota_image_check_head(gateway_head, EXAMPLE_LEN, run32, sizeof(run32), NULL), OTA_IMAGE_OK);
    /* Non-printable bytes are not passed to the log. */
    memcpy(h, gateway_head, sizeof(h));
    h[100] = 0x1B;
    CHECK_EQ(check_exact(h, sizeof(h), GATEWAY, &info), OTA_IMAGE_OK);
    CHECK(strcmp(info.project, "airplay-gateway_v4.5?49") == 0);
}

static void test_progress(void)
{
    int last = 0, n = 0, prev = 0, bad = 0;
    for (size_t done = 0; done < SPEAKER_LEN;) {
        done += 4096;
        if (done > SPEAKER_LEN) done = SPEAKER_LEN;
        int p = ota_image_progress(done, SPEAKER_LEN, &last);
        if (p >= 0) {
            n++;
            if (p != prev + 10) bad++;
            prev = p;
        }
    }
    CHECK_EQ(n, 9);       /* 10 % to 90 %: the end line takes the place of 100 % */
    CHECK_EQ(prev, 90);
    CHECK_EQ(bad, 0);

    last = 0;
    CHECK_EQ(ota_image_progress(288, 288, &last), -1);
    CHECK_EQ(ota_image_progress(1000, 1000, &last), -1);
    last = 0;
    CHECK_EQ(ota_image_progress(50, 1000, &last), -1);
    CHECK_EQ(ota_image_progress(250, 1000, &last), 20); /* a jump logs once */
    CHECK_EQ(ota_image_progress(299, 1000, &last), -1);
    CHECK_EQ(ota_image_progress(950, 1000, &last), 90);
    CHECK_EQ(ota_image_progress(5000, 1000, &last), -1);
    CHECK_EQ(ota_image_progress(1, 0, &last), -1);
    CHECK_EQ(ota_image_progress(1, 1, NULL), -1);
    last = 0;
    CHECK_EQ(ota_image_progress((size_t)-1 / 4, (size_t)-1, &last), 20);
}

static void test_result_strings(void)
{
    for (int r = OTA_IMAGE_OK; r <= OTA_IMAGE_OTHER_KIND; r++) {
        CHECK(ota_image_result_str((ota_image_result_t)r) != NULL);
    }
}

int main(void)
{
    load_example("gateway_head.bin", gateway_head);
    load_example("speaker_head.bin", speaker_head);
    load_example("not_firmware.bin", not_firmware);

    RUN(test_layout_constants);
    RUN(test_ex1_gateway_on_gateway);
    RUN(test_ex2_ex3_other_kind);
    RUN(test_ex4_not_firmware);
    RUN(test_ex5_chip_esp32);
    RUN(test_ex6_ex7_sizes);
    RUN(test_ex8_other_version);
    RUN(test_ex10_no_hash);
    RUN(test_ex11_running_without_kind);
    RUN(test_short_buffers);
    RUN(test_damaged_headers);
    RUN(test_kind_edge_cases);
    RUN(test_progress);
    RUN(test_result_strings);
    return test_summary("test_ota_image");
}
