# Third-Party Modified Version of the Fraunhofer FDK AAC Codec Library for Android

`fdk-aac/` holds the **decoder** libraries of fdk-aac v2.0.3
([github.com/mstorsjo/fdk-aac](https://github.com/mstorsjo/fdk-aac), tag `v2.0.3`,
commit `716f439`): libAACdec, libArithCoding, libDRCdec, libFDK, libMpegTPDec,
libPCMutils, libSACdec, libSBRdec, libSYS, plus `NOTICE` (the license) and
`ChangeLog`. The encoder libraries are not included.

The license terms are in [`fdk-aac/NOTICE`](fdk-aac/NOTICE). As it requires,
this is a modified version: it is a **Third-Party Modified Version of the
Fraunhofer FDK AAC Codec Library for Android**, not the original library.

## Changes (2026-10-24)

All changes are in `libAACdec`, each one marked in the source with the comment
`airplay-esp32 patch`, and each changed file carries a dated notice under its
header. The unmodified import is git commit `8667b02`, in
`components/fdk_aac/fdk-aac` (moved to `speaker/components/fdk_aac` later);
`git diff -M 8667b02 -- components/fdk_aac/fdk-aac speaker/components/fdk_aac/fdk-aac`
shows every change.

| File | Change |
|---|---|
| `include/aacdecoder_lib.h` | New API: `aacDecoder_SetSynthChannel()`, `aacDecoder_SetTimeHook()`, `aacDecoder_GetSynthInfo()`. |
| `src/aacdecoder.h` | Fields in `AAC_DECODER_INSTANCE` for the selected channel, the per-plane channel labels, the single-channel output flag and the synthesis timestamps. |
| `src/aacdecoder.cpp` | Synthesis (concealment, DRC, IMDCT, time-domain fading) runs only for the channel selected by label; the others are still parsed but their planes are neither synthesized nor cleared. Plane labels are remembered per frame. Timestamps at the start and end of synthesis. |
| `src/aacdecoder_lib.cpp` | When exactly one channel was synthesized and nothing needs the others (no SBR/MPS, limiter, DRC level normalization or USAC), skip `pcmDmx` and write only that plane, mono, scaled like the normal output path. Implementation of the new API. |

Purpose: an ESP32-S3 plays one speaker of a 5.1/7.1 AirPlay stream and cannot
synthesize all eight channels of AAC-LC in real time. Selecting nothing
(`ACT_NONE`, the default) keeps the stock behaviour.

The single-channel output was checked on a host against the full decode:
7.1 AAC from the FDK encoder decodes bit-identically through both paths for
each of the 8 channels.

## Changes (2026-10-26)

`libAACdec/include/aacdecoder_lib.h` and `src/aacdecoder_lib.cpp` add
`aacDecoder_DecodeFrame32()`. It shares the original decoder, history and control
logic, but exports signed 32-bit PCM from `PCM_DEC` before the stock `INT_PCM`
16-bit rounding. Both full interleaved output and single-channel synthesis use
the same scaling as the original output. Full output retains the existing DRC
normalization, channel mapping and saturation. Invalid output is cleared using
the appropriate sample width, and null/nonpositive output buffers are rejected.
The original S16 API remains available; `INT_PCM` and the library's other
16-bit APIs have not been redefined.

The new API requires the stock S16-output PCM limiter to be disabled and rejects
AAC-SLS output; it never silently substitutes an upshifted S16 result. The
application already disables the limiter and advertises AAC-LC. AAC is lossy:
the output width describes decoder precision, not a recoverable original PCM
bit depth. The ESP32 wrapper uses this API for stereo as well as surround.

## Changes (2026-10-26, speed-ups)

| File | Change |
|---|---|
| `libFDK/include/clz.h`, new `libFDK/include/clz_builtin.h` | On Xtensa (`__XTENSA__`, or `FDK_USE_BUILTIN_CLZ` on a host), `fixnormz_D`/`fixnorm_D` use `__builtin_clz`/`__builtin_clrsb` (NSAU/NSA on the ESP32-S3) instead of FDK's one-bit-per-iteration fallback loops. Identical results for all 2^32 inputs. |
| `libAACdec/src/channel.h`, `channel.cpp` | `CChannelElement_Read()` takes `skipSpectralDecode`; when set, the element's bits are still read but its spectrum is not inverse quantized. |
| `libAACdec/src/aacdecoder.h`, `aacdecoder.cpp`, `aacdecoder_lib.cpp` | With a channel selected by `aacDecoder_SetSynthChannel()`, elements that do not hold it skip inverse quantization and `CChannelElement_Decode()` (M/S, intensity, PNS, TNS). The element is found from the previous frame's synthesis; a new selection decodes all elements until its channel has been synthesized once. CCE parsing (dummy channel and work buffers, ~35 KiB of stack) moved from `CAacDecoder_DecodeFrame()` into a separate non-inlined function, so that stack is only used when a CCE element occurs; `CAacDecoder_DecodeFrame()` itself drops from 35.6 KiB to ~1 KiB (Xtensa `-fstack-usage`). |

Checked on a host build that emulates the ESP32 (no x86 helpers): FDK-encoded
7.1, 5.1 and stereo AAC-LC decode bit-identically to the previous version for
every speaker, with full output, and while switching speakers every 37 frames.
Instruction counts for one speaker of 7.1 dropped by about half (clz ~32 %,
element skip a further ~27-37 %); this is a host measurement, not ESP32 timing.
On the ESP32-S3 (one back speaker of 7.1) the average decode time dropped from
about 11.3 ms to 7.2 ms per frame, and the decode task's deepest stack use
from ~45 KiB to ~11 KiB.

| File | Change (2026-10-26, later the same day) |
|---|---|
| `libAACdec/src/aacdecoder.cpp` | The CCE parsing helper takes its dummy channel and work buffers (~35 KiB) from the heap (`FDKaalloc`/`FDKafree`) for the duration of one CCE element instead of from the stack. If that memory is not available the frame is concealed (`AAC_DEC_DECODE_FRAME_ERROR`). This lets the decoder run on a 20 KiB stack. |

## Changes (2026-10-26, memory)

| File | Change |
|---|---|
| `libAACdec/src/aac_ram.cpp` | The time-data work buffer (`WorkBufferCore5`) is `8 * 2048` samples (64 KiB) instead of `8 * 4096 * 2` (256 KiB): enough for 8 channels of AAC-LC and of SBR output. Every user of it checks the size and returns an error rather than writing past it, so a stream that would need more fails cleanly (e.g. DRC deinterleaving of an 8-channel HE-AAC stream). |
| `libAACdec/src/aacdecoder_lib.cpp` | The PCM limiter (~46 KiB) is no longer created in `aacDecoder_Open()` but on first use: by the first frame that has the limiter enabled, or by `AAC_PCM_LIMITER_ATTACK_TIME`/`RELEAS_TIME`. A decoder that disables the limiter never allocates it. Without memory such a frame is output unlimited. |
| `libMpegTPDec/include/tpdec_lib.h`, `src/tpdec_lib.cpp`, `src/tpdec_adts.cpp` | The transport input bit buffer is `TPDEC_INBUF_BYTES` = 16 KiB instead of 32 KiB, used for the allocation, the bit-buffer size and the frame-length and bit-reservoir checks. It still holds two of the largest ADTS frames. |

## Changes (2026-10-26, headroom shift)

| File | Change |
|---|---|
| `libAACdec/src/aacdecoder_lib.cpp` | The headroom shift of the decoded planes (`pTimeData2[i] = pTimeData3[i] >> (PCM_OUT_HEADROOM - timeDataHeadroom)`) is postponed when one channel is selected on plain AAC (no SBR, MPS, USAC, RSVD60 or AAC-SLS), because nothing reads the planes until the output path is chosen. The single-channel output then shifts only the plane it plays (1 of up to 8); any other output shifts every plane, as before. Same operation on the same samples, so the output is unchanged. |

## No patent license

As `NOTICE` section 3 says, the FDK license grants no patent rights. AAC
patent licenses are available through Via Licensing or the patent owners;
using this code is your responsibility in that respect.
