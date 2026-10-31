/* airplay-esp32 patch (2026-10-26), part of the Third-Party Modified Version
 * of the Fraunhofer FDK AAC Codec Library for Android in
 * esp32-airplay2-surround; see ../../NOTICE and
 * components/fdk_aac/MODIFICATIONS.md.
 *
 * FDK ships leading-bit helpers for ARM, MIPS, x86 and PowerPC only. On
 * Xtensa (ESP32) clz.h fell back to loops that test one bit per iteration;
 * these are hot in inverse quantization. The ESP32-S3 core has NSA/NSAU
 * (one instruction each), which GCC emits for these builtins. Results are
 * identical to the generic fallbacks for every 32-bit input, including 0. */

#ifndef CLZ_BUILTIN_H
#define CLZ_BUILTIN_H

/* Leading zero bits of a 32-bit value; 32 for 0 (generic fixnormz_D). */
#define FUNCTION_fixnormz_D
inline INT fixnormz_D(LONG value) {
  return value ? (INT)__builtin_clz((unsigned int)value) : 32;
}

/* Leading sign bits minus 1 (redundant sign bits); 0 for 0 (generic
 * fixnorm_D). */
#define FUNCTION_fixnorm_D
inline INT fixnorm_D(LONG value) {
  return value ? (INT)__builtin_clrsb((int)value) : 0;
}

#endif /* CLZ_BUILTIN_H */
