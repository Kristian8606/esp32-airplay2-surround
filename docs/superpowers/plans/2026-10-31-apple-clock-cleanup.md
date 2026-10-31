# Standalone Apple clock implementation plan

**Goal:** Finish the existing design's stage 4: the Gateway runs only apple_clock, with no comparison clock, test hooks or one-way fallback.

**Spec:** `docs/apple-clock-design.md`, superseded by the user's explicit request to remove fallback and test paths.

**Architecture:** One task owns both PTP sockets, transport state and the mathematical core. RTSP selects/releases the peer; TIME, anchor diagnostics and sync_pulse read the same published line. Speakers retain their external TIME receiver.

**Constraints:** Work in the existing test branch and preserve unrelated working changes. No flashing, commits or added firmware test modes. Temporary validation programs live in `/tmp/apple-clock-cleanup`.

## Tasks

- [x] Reconcile core/header; keep strict RTT filtering and continuity, remove Sync-only fallback. Verify acquisition, poor RTT traffic, silence and clock steps with a temporary host program.
- [x] Give apple_clock ownership of UDP 319/320, matching by peer/domain/source/port/sequence, including Announce-before-Sync, session changes and packet length validation.
- [x] Route Gateway consumers to apple_clock; remove gw_timeline, PTP probes, test code and selector options. Restrict ptp_clock to speakers' external TIME state and remove the passive estimator.
- [x] Build Gateway and speaker, inspect linked symbols and review the diff; update version/design status with the actual result.

## Review focus

Malformed packets must not reach timestamp parsers. Pending Delay_Resp must not survive peer/GM changes. An Announce before Sync must remain known. Same-GM relay changes should preserve the line. Poor RTT observations must not replace precise points or end an otherwise live clock. TIME must never become valid before Announce.

## Validation record

- Both ESP-IDF builds pass for v4.5.32; no board flash performed.
- Gateway ELF and compilation database contain apple_clock and no ptp_clock, old engine, timeline or PTP probe/test source.
- Temporary host checks cover 30 minutes of jitter, positive/negative steps, improving RTT floor, continued poor-RTT traffic, silence expiry, packet identity/GM transitions, truncated packets and overflow corrections, and speaker TIME carry/GM epochs. UndefinedBehaviorSanitizer checks pass. The old poor-RTT timeout and lower-floor interruption were reproduced before their fixes.
- Both Gateway and speaker sync_pulse translation units compile with the optional GPIO feature enabled.
- Independent review found the remaining speaker readiness constant and the improving-floor interruption; both are fixed and verified.

## Decisions

- The user's cleanup request supersedes the original design's one-way fallback and parallel comparison stages.
- Live but rejected RTT samples keep the existing line alive without changing its fitted points. Complete exchange silence still expires it after 30 seconds. This prevents the reproduced dropout under load; actual phase accuracy still needs measurement on hardware.
- Existing optional oscilloscope sync_pulse remains as a measurement tool and reads the same active line; PTP experiment/probe/simulation source is removed from the repository.
