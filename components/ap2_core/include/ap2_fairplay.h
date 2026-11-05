#pragma once

#include <stddef.h>
#include <stdint.h>

/* FairPlay setup (POST /fp-setup). Apple senders run it before they send
 * audio; with AirPlay 2 pairing the audio key comes over the paired channel,
 * so the receiver only has to give the answers a sender expects. There are
 * two exchanges, each message opening with a 12-byte header: "FPLY",
 * version 3, 1, the message number, four zero bytes and the length of the
 * rest.
 *
 * - message 1 (16 bytes, mode 0 to 3 in byte 14): answered with the fixed
 *   142-byte reply of that mode;
 * - message 3 (164 bytes): answered with a fixed 12-byte header and the last
 *   20 bytes of the message (32 bytes).
 *
 * The replies are fixed interop data, see NOTICE. */

#define AP2_FAIRPLAY_REPLY_MAX 142

/* Writes the reply to one /fp-setup body. Returns its length (142 or 32),
 * or -1 if the body is not FairPlay version 3, is too short, has a mode
 * over 3 or a message number other than 1 or 3, or the reply does not fit
 * reply_capacity. */
int ap2_fairplay_setup(const uint8_t *request, size_t request_len,
                       uint8_t *reply, size_t reply_capacity);
