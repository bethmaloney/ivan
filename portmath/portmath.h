/* portmath -- the math functions game logic is allowed to call.
 *
 * These exist so that a native build and a WASM build compute the same
 * numbers. The host libm cannot give that guarantee: sin, cos, log10, pow and
 * atan are not correctly rounded by any standard, so glibc and Emscripten's
 * musl-derived libm disagree in the last ulp -- measured at 473 of 27,496
 * calls across the two committed corpora, all by one ulp. World generation
 * feeds those results through int() and (short) truncation, so one ulp becomes
 * a different tile, a different continent, a different game.
 *
 * See portmath/README.md for the measurement and docs/port-log.md §6.3.
 *
 * Use these in anything that reaches game state or the framebuffer. Calling
 * <cmath> directly from Main/ or FeLib/ reintroduces the divergence silently,
 * which is why portmath/check-callers.py exists to catch it.
 */
#ifndef __PORTMATH_H__
#define __PORTMATH_H__

extern "C"
{
  double pm_sin(double);
  double pm_cos(double);
  void   pm_sincos(double, double*, double*);
  double pm_log(double);
  double pm_log10(double);
  double pm_pow(double, double);
  double pm_atan(double);
  double pm_hypot(double, double);
  double pm_exp(double);
}

namespace portmath
{
  inline double Sin(double X) { return pm_sin(X); }
  inline double Cos(double X) { return pm_cos(X); }
  inline double Log(double X) { return pm_log(X); }
  inline double Log10(double X) { return pm_log10(X); }
  inline double Pow(double X, double Y) { return pm_pow(X, Y); }
  inline double Atan(double X) { return pm_atan(X); }
  inline double Hypot(double X, double Y) { return pm_hypot(X, Y); }
  inline double Exp(double X) { return pm_exp(X); }

  /* Call this rather than Sin(X) and Cos(X) separately. GCC rewrites that pair
     into a single sincos() and glibc's sincos is not obliged to agree with its
     own sin and cos, so whether the results match depends on whether the
     compiler happened to fuse them -- a portability hazard with no visible
     call site. Naming the fused form makes the choice explicit and identical
     everywhere. */
  inline void SinCos(double X, double* S, double* C) { pm_sincos(X, S, C); }
}

#endif
