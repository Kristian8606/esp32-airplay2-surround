#pragma once

/* What each speaker role plays from a source, as one table for the
 * Gateway's player (ROADMAP_USB_PLAYER.md, phase 0 section 2). The speakers
 * do the same for Apple streams today (spk_link.c apply_stereo_route,
 * aac_decoder.c); this header does not change them.
 *
 * Roles are aac_speaker_t: -1 mono mix, 0 L, 1 R, 2 C, 3 LFE, 4 Ls, 5 Rs,
 * 6 Lrs, 7 Rrs. Source channels are in the same internal order, so a role's
 * own channel has the role's index. A mono source is in slot 0 (L). */

#include <stdbool.h>
#include <stdint.h>

#define SURROUND_ROLE_MIX (-1)
#define SURROUND_ROLE_LFE 3
#define SURROUND_ROLES 9 /* -1..7 */

typedef enum {
  SURROUND_SIG_NONE = 0,    /* nothing: the speaker gets no lane */
  SURROUND_SIG_CHANNEL,     /* one source channel, bit for bit */
  SURROUND_SIG_HALF_LR,     /* 0.5 * (L + R), computed: 24 bit */
  SURROUND_SIG_MEAN_NO_LFE, /* mean of all channels but LFE, computed: 24 bit */
} surround_sig_kind_t;

typedef struct {
  surround_sig_kind_t kind;
  uint8_t channel; /* SURROUND_SIG_CHANNEL: source slot */
} surround_sig_t;

/* source_channels: 1, 2, 6 (5.1) or 8 (7.1); present: bit n set when slot
 * n carries a channel (5.1 has no Lrs/Rrs). */
static inline surround_sig_t surround_role_signal(int role, int source_channels,
                                                  uint8_t present) {
  const surround_sig_t none = {SURROUND_SIG_NONE, 0};
  if (source_channels == 1) {
    /* M to L, R, the subwoofer and a mono mix, bit for bit (decision 8.4) */
    if (role == 0 || role == 1 || role == SURROUND_ROLE_LFE || role == SURROUND_ROLE_MIX)
      return (surround_sig_t){SURROUND_SIG_CHANNEL, 0};
    return none;
  }
  if (source_channels == 2) {
    if (role == 0 || role == 1) return (surround_sig_t){SURROUND_SIG_CHANNEL, (uint8_t)role};
    if (role == SURROUND_ROLE_LFE || role == SURROUND_ROLE_MIX)
      return (surround_sig_t){SURROUND_SIG_HALF_LR, 0};
    return none; /* centre and surrounds stay silent with a stereo source */
  }
  /* 5.1 / 7.1: every channel on its own speaker, no mixing (decisions 5, 7);
   * a mono mix speaker plays the mean of the non-LFE channels (8.3). */
  if (role == SURROUND_ROLE_MIX) return (surround_sig_t){SURROUND_SIG_MEAN_NO_LFE, 0};
  if (role >= 0 && role < 8 && (present & (1U << role)))
    return (surround_sig_t){SURROUND_SIG_CHANNEL, (uint8_t)role};
  return none;
}

static inline const char *surround_role_name(int role) {
  static const char *const names[SURROUND_ROLES] = {"mix", "L", "R", "C", "LFE",
                                                    "Ls", "Rs", "Lrs", "Rrs"};
  return role >= -1 && role < 8 ? names[role + 1] : "?";
}
