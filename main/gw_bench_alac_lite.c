/* USB player, phase 0b: a light mono ALAC encoder. See gw_bench_alac_lite.h.
 *
 * The bitstream is standard ALAC as Apple's reference decoder reads it
 * (alac, Apache License 2.0: ALACDecoder.cpp, dp_dec.c unpc_block,
 * ag_dec.c dyn_decomp), so the speakers' esp_alac_dec decodes it unchanged:
 *
 *   SCE element: tag 0 (3 bits), instance 0 (4), 12 zero bits,
 *   partialFrame 0 (1), bytesShifted (2), escape (1);
 *   compressed: mixBits 0 (8), mixRes 0 (8), mode 0 (4), denShift (4),
 *   pbFactor 4 (3), order (5), the order's initial coefficients (16 each),
 *   at 24 bit the shifted-off low bytes (8 bits per sample), then the
 *   residuals in adaptive Golomb code with the cookie's mb/pb/kb 10/40/14;
 *   escape: the samples raw; then ID_END (3) and byte alignment.
 *
 * What makes it light against the reference encoder: one predictor order,
 * no search for the best order or shift, one pass over the frame. The
 * decoder adapts the predictor's coefficients sample by sample (sign LMS);
 * this encoder runs the same adaptation on the same samples, and the next
 * frame starts from where the last one ended (its header carries them), so
 * the predictor keeps converging across frames instead of being searched
 * for in every frame. 24 bit is coded like the reference: the low byte
 * raw, the upper 16 bits predicted (bytesShifted 1). */

#include "gw_bench_alac_lite.h"

#include <string.h>

#define LITE_MAX_N    1024U
#define LITE_DENSHIFT 9
#define LITE_PB_FACTOR 4U  /* pb = 40 * 4 / 4: the cookie's 40 */
/* aglib.h */
#define AG_QBSHIFT 9U
#define AG_QB (1U << AG_QBSHIFT)
#define AG_MB0 10U
#define AG_PB0 40U
#define AG_KB0 14U
#define AG_MMULSHIFT 2U
#define AG_MDENSHIFT (AG_QBSHIFT - AG_MMULSHIFT - 1U)
#define AG_MOFF (1U << (AG_MDENSHIFT - 2U))
#define AG_BITOFF 24
#define AG_MAX_PREFIX_16 9U
#define AG_MAX_PREFIX_32 9U
#define AG_MAX_DATATYPE_BITS_16 16U
#define AG_N_MAX_MEAN_CLAMP 0xffffU
#define AG_N_MEAN_CLAMP_VAL 0xffffU
#define ALAC_ID_SCE 0U
#define ALAC_ID_END 7U

/* ---- bit writer, most significant bit first ---- */

typedef struct {
  uint8_t *p;
  size_t cap, pos;
  uint64_t acc;
  int n; /* bits in acc not yet written, < 8 between calls */
  bool full;
} bw_t;

static inline void bw_put(bw_t *w, uint32_t v, unsigned nbits) {
  if (nbits < 32U) v &= (1U << nbits) - 1U;
  w->acc = (w->acc << nbits) | v;
  w->n += (int)nbits;
  while (w->n >= 8) {
    w->n -= 8;
    if (w->pos < w->cap) w->p[w->pos++] = (uint8_t)(w->acc >> w->n);
    else w->full = true;
  }
}

static void bw_align(bw_t *w) {
  if (w->n) bw_put(w, 0, (unsigned)(8 - w->n));
}

/* ---- adaptive Golomb (ag_enc.c dyn_comp) ---- */

static inline uint32_t lead(uint32_t m) { return m ? (uint32_t)__builtin_clz(m) : 32U; }
static inline uint32_t lg3a(uint32_t x) { return 31U - lead(x + 3U); }

static void ag_encode(bw_t *w, const int32_t *pc, uint32_t num, uint32_t bitsize) {
  const uint32_t pb = AG_PB0 * LITE_PB_FACTOR / 4U, kb = AG_KB0, wb = (1U << AG_KB0) - 1U;
  uint32_t mb = AG_MB0, c = 0, zmode = 0;
  while (c < num) {
    uint32_t k = lg3a(mb >> AG_QBSHIFT);
    if (k > kb) k = kb;
    const uint32_t m = (1U << k) - 1U;
    const int32_t del = pc[c++];
    const uint32_t a = (uint32_t)(del < 0 ? -del : del);
    const uint32_t n = (a << 1) - ((uint32_t)(del >> 31) & 1U) - zmode;
    const uint32_t div = n / m;
    bool coded = false;
    if (div < AG_MAX_PREFIX_32) {
      const uint32_t mod = n - m * div, de = mod == 0U;
      const uint32_t nb = div + k + 1U - de;
      if (nb <= 25U) {
        bw_put(w, (((1U << div) - 1U) << (nb - div)) + mod + 1U - de, nb);
        coded = true;
      }
    }
    if (!coded) {
      bw_put(w, (1U << AG_MAX_PREFIX_32) - 1U, AG_MAX_PREFIX_32);
      bw_put(w, n, bitsize);
    }
    mb = pb * (n + zmode) + mb - ((pb * mb) >> AG_QBSHIFT);
    if (n > AG_N_MAX_MEAN_CLAMP) mb = AG_N_MEAN_CLAMP_VAL;
    zmode = 0;
    if (((mb << AG_MMULSHIFT) < AG_QB) && c < num) {
      zmode = 1;
      uint32_t nz = 0;
      while (c < num && pc[c] == 0) {
        ++c;
        if (++nz >= 65535U) {
          zmode = 0;
          break;
        }
      }
      const uint32_t kz = lead(mb) - AG_BITOFF + ((mb + AG_MOFF) >> AG_MDENSHIFT);
      const uint32_t mz = ((1U << kz) - 1U) & wb;
      const uint32_t dz = nz / mz;
      uint32_t nb, val;
      if (dz >= AG_MAX_PREFIX_16) {
        nb = AG_MAX_PREFIX_16 + AG_MAX_DATATYPE_BITS_16;
        val = (((1U << AG_MAX_PREFIX_16) - 1U) << AG_MAX_DATATYPE_BITS_16) + nz;
      } else {
        const uint32_t mod = nz % mz, de = mod == 0U;
        nb = dz + kz + 1U - de;
        val = (((1U << dz) - 1U) << (nb - dz)) + mod + 1U - de;
        if (nb > AG_MAX_PREFIX_16 + AG_MAX_DATATYPE_BITS_16) {
          nb = AG_MAX_PREFIX_16 + AG_MAX_DATATYPE_BITS_16;
          val = (((1U << AG_MAX_PREFIX_16) - 1U) << AG_MAX_DATATYPE_BITS_16) + nz;
        }
      }
      bw_put(w, val, nb);
      mb = 0;
    }
  }
}

/* ---- predictor: the encoder side of dp_dec.c unpc_block ---- */

static inline int32_t sext(int32_t v, uint32_t sh) {
  return (int32_t)((uint32_t)v << sh) >> sh;
}

static inline int32_t sign_of(int32_t i) { return (i > 0) - (i < 0); }

/* Residuals r of x, adapting c exactly as the decoder will. All products
 * and sums wrap at 32 bits like the reference's int32 arithmetic. */
static void predict(const int32_t *x, int32_t *r, uint32_t num, int16_t *c, int order,
                    uint32_t chanbits) {
  const uint32_t sh = 32U - chanbits;
  const int32_t half = 1 << (LITE_DENSHIFT - 1);
  r[0] = x[0];
  const uint32_t lim = (uint32_t)order + 1U;
  for (uint32_t j = 1; j < lim && j < num; ++j) r[j] = sext(x[j] - x[j - 1], sh);
  for (uint32_t j = lim; j < num; ++j) {
    const int32_t top = x[j - lim];
    const int32_t *pout = x + j - 1;
    uint32_t sum = 0;
    for (int k = 0; k < order; ++k) sum += (uint32_t)((int32_t)c[k] * (pout[-k] - top));
    const int32_t p = (int32_t)(sum + (uint32_t)half) >> LITE_DENSHIFT;
    const int32_t del = sext((int32_t)((uint32_t)x[j] - (uint32_t)top - (uint32_t)p), sh);
    r[j] = del;
    int32_t del0 = del;
    if (del > 0) {
      for (int k = order - 1; k >= 0; --k) {
        const int32_t dd = top - pout[-k];
        const int32_t sgn = sign_of(dd);
        c[k] = (int16_t)(c[k] - sgn);
        del0 -= (order - k) * ((sgn * dd) >> LITE_DENSHIFT);
        if (del0 <= 0) break;
      }
    } else if (del < 0) {
      for (int k = order - 1; k >= 0; --k) {
        const int32_t dd = top - pout[-k];
        const int32_t sgn = sign_of(dd);
        c[k] = (int16_t)(c[k] + sgn);
        del0 -= (order - k) * ((-sgn * dd) >> LITE_DENSHIFT);
        if (del0 >= 0) break;
      }
    }
  }
}

void alac_lite_init(alac_lite_t *e, int bits, int order) {
  memset(e, 0, sizeof(*e));
  e->bits = bits;
  e->order = order == 8 ? 8 : 4;
  e->coefs[0] = 1 << LITE_DENSHIFT; /* start as "the previous sample" */
}

static size_t escape_frame(const alac_lite_t *e, const int32_t *pcm, uint32_t n, uint8_t *out,
                           size_t cap) {
  bw_t w = {.p = out, .cap = cap};
  bw_put(&w, ALAC_ID_SCE, 3);
  bw_put(&w, 0, 4);
  bw_put(&w, 0, 12);
  bw_put(&w, 1, 4); /* partial 0, shift 0, escape 1 */
  for (uint32_t i = 0; i < n; ++i) bw_put(&w, (uint32_t)pcm[i], (unsigned)e->bits);
  bw_put(&w, ALAC_ID_END, 3);
  bw_align(&w);
  return w.full ? 0 : w.pos;
}

size_t alac_lite_encode(alac_lite_t *e, const int32_t *pcm, uint32_t n, uint8_t *out,
                        size_t cap, bool *escaped) {
  static int32_t x[LITE_MAX_N], r[LITE_MAX_N]; /* internal RAM; one caller at a time */
  if (escaped) *escaped = false;
  if (n == 0 || n > LITE_MAX_N) return 0;
  const uint32_t shift = e->bits == 24 ? 1U : 0U;
  const uint32_t chanbits = (uint32_t)e->bits - shift * 8U;
  for (uint32_t i = 0; i < n; ++i) x[i] = shift ? pcm[i] >> 8 : pcm[i];
  int16_t c0[8];
  memcpy(c0, e->coefs, sizeof(c0));
  predict(x, r, n, e->coefs, e->order, chanbits);

  bw_t w = {.p = out, .cap = cap};
  bw_put(&w, ALAC_ID_SCE, 3);
  bw_put(&w, 0, 4);
  bw_put(&w, 0, 12);
  bw_put(&w, shift << 1, 4); /* partial 0, bytesShifted, escape 0 */
  bw_put(&w, 0, 8);          /* mixBits */
  bw_put(&w, 0, 8);          /* mixRes */
  bw_put(&w, (0U << 4) | LITE_DENSHIFT, 8);
  bw_put(&w, (LITE_PB_FACTOR << 5) | (uint32_t)e->order, 8);
  for (int k = 0; k < e->order; ++k) bw_put(&w, (uint16_t)c0[k], 16);
  if (shift)
    for (uint32_t i = 0; i < n; ++i) bw_put(&w, (uint32_t)pcm[i] & 0xffU, 8);
  ag_encode(&w, r, n, chanbits);
  bw_put(&w, ALAC_ID_END, 3);
  bw_align(&w);

  const size_t raw = ((size_t)n * (size_t)e->bits + 23U + 3U + 7U) / 8U;
  if (w.full || w.pos >= raw) {
    if (escaped) *escaped = true;
    return escape_frame(e, pcm, n, out, cap);
  }
  return w.pos;
}
