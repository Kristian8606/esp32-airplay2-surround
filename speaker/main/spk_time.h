#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "surround_proto.h"

/* Speaker: turns the Gateway's TIME messages into this ESP's
 * estimate of the sender clock (ptp_clock_set_external()), which audio reads
 * as if it came from the sender's PTP. Called from the spk_link task only.
 *
 * The Gateway says "sender PTP time P is TSF T". The speaker reads its own
 * TSF and clock together, keeps a line through those readings (tsf_fit.h)
 * and finds the local time of TSF T on it: offset = P - local(T). How long
 * the message took to arrive does not matter. */
void spk_time_handle(const surround_time_t *m);

/* For STATUS: true while the Gateway's time is in use; rms of the TSF fit. */
bool spk_time_status(uint32_t *tsf_rms_ns);

/* This ESP's clock (esp_timer, ns) as the access point's TSF, from the same
 * line the TIME messages use. False until the fit has enough samples. */
bool spk_time_tsf_at(int64_t local_ns, int64_t *tsf_ns);

/* The link to the Gateway ended: withdraw the estimate. */
void spk_time_reset(void);
