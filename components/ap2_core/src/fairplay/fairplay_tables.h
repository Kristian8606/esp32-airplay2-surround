#pragma once

#include <stdint.h>

#define FP_SETUP_MODES      4
#define FP_SETUP_REPLY_LEN  142
#define FP_SETUP_HEADER_LEN 12

/* The fixed FairPlay setup replies (fairplay_tables.c, see NOTICE). */
extern const uint8_t fp_setup_replies[FP_SETUP_MODES][FP_SETUP_REPLY_LEN];
extern const uint8_t fp_setup_header[FP_SETUP_HEADER_LEN];
