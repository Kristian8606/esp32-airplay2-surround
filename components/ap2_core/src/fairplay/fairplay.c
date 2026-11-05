#include "ap2_fairplay.h"

#include <string.h>

#include "fairplay_tables.h"

/* Header fields of every FairPlay setup message. */
#define FP_MAGIC        "FPLY"
#define FP_VERSION_POS  4
#define FP_VERSION      3
#define FP_SEQ_POS      6
#define FP_MODE_POS     14
#define FP_SUFFIX_LEN   20

#define FP_SEQ_SETUP1   1 /* mode request, answered by message 2 */
#define FP_SEQ_SETUP3   3 /* key message, answered by message 4 */

int ap2_fairplay_setup(const uint8_t *request, size_t request_len,
                       uint8_t *reply, size_t reply_capacity) {
  if (request == NULL || reply == NULL || request_len < FP_SETUP_HEADER_LEN ||
      memcmp(request, FP_MAGIC, 4) != 0 ||
      request[FP_VERSION_POS] != FP_VERSION) {
    return -1;
  }

  switch (request[FP_SEQ_POS]) {
    case FP_SEQ_SETUP1: {
      if (request_len <= FP_MODE_POS) return -1;
      const uint8_t mode = request[FP_MODE_POS];
      if (mode >= FP_SETUP_MODES || reply_capacity < FP_SETUP_REPLY_LEN) {
        return -1;
      }
      memcpy(reply, fp_setup_replies[mode], FP_SETUP_REPLY_LEN);
      return FP_SETUP_REPLY_LEN;
    }
    case FP_SEQ_SETUP3: {
      const size_t len = FP_SETUP_HEADER_LEN + FP_SUFFIX_LEN;
      if (request_len < len || reply_capacity < len) return -1;
      memcpy(reply, fp_setup_header, FP_SETUP_HEADER_LEN);
      memcpy(reply + FP_SETUP_HEADER_LEN,
             request + request_len - FP_SUFFIX_LEN, FP_SUFFIX_LEN);
      return (int)len;
    }
    default:
      return -1;
  }
}
