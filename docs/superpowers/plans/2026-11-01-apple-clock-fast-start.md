# Apple clock first-exchange startup

**Goal:** Remove the reproduced 32.5-second audio startup wait without adding another clock or a one-way fallback.

**Design:** A first complete two-way exchange with RTT at most 3 ms seeds phase immediately, using the remembered rate (or zero after a cold boot). It starts the existing published line before its first one-second fit bucket closes. Normal strict per-second filtering, long-term rate learning, phase slew, and confirmed-step rules then continue on that same line. Slower initial paths retain the existing multi-point qualification. A new precise exchange can seed acquisition even if earlier slow startup points exist. Announce gating remains mandatory.

**Tradeoff:** A first 3 ms exchange has up to +/-1.5 ms path-asymmetry uncertainty under the four-timestamp model. There is no guarantee of zero total startup latency: Announce, the first suitable reply, TIME delivery, audio anchors and playout priming are still required. The threshold includes the captured 2.46 ms first exchange and the design's observed Apple TV/iPhone minima. No change to waiting-for-FLUSH speaker admission.

**Interfaces:** Core publishes quality-checked exchange count separately from fitted-second count. Gateway TIME sample_count uses exchanges, so the existing realtime four-sample/400 ms qualification does not accidentally wait for four accepted seconds. Protocol layout and speaker code remain unchanged.

- [x] Reproduce failure with a temporary host check outside the repository.
- [x] Seed the existing estimator/publication from the first suitable exchange; preserve pause/rejoin rate and phase continuity.
- [x] Publish the exchange count to TIME and preserve Announce/identity guards.
- [x] Run temporary startup, clock-step, jitter, silence, packet and speaker compatibility checks; build both firmwares and review.

**Constraints:** Existing dirty test branch; no firmware test modes, no one-way fallback, no flashing or commits. Temporary checks in `/tmp/apple-clock-fast-start`.

## Verification

The temporary first-exchange check failed on v4.5.32 and passes on v4.5.33. It covers immediate phase seeding, scarce accepted seconds, known/unknown rate, silence/rejoin, rejection of slow single replies, higher-RTT multi-point acquisition, and four realtime observations within 375 ms. Integrated core/TIME/external-receiver qualification passes at 500 ms. The prior 30-minute jitter/positive-and-negative-step simulation, improved-floor continuity, poor-RTT liveness, and transport identity/malformed-packet checks pass under UndefinedBehaviorSanitizer. Independent review found no actionable issues. Both firmware builds pass as v4.5.33 and their ELFs contain no legacy estimator/test symbols. No hardware flash or claim of measured on-board startup latency.
