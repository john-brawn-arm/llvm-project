//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

_CLC_DEF _CLC_OVERLOAD float __clc_remquo(float x, float y,
                                          __CLC_ADDRESS_SPACE int *quo) {
  x = __clc_flush_denormal_if_not_supported(x);
  y = __clc_flush_denormal_if_not_supported(y);
  int ux = __clc_as_int(x);
  int ax = ux & EXSIGNBIT_SP32;
  float xa = __clc_as_float(ax);
  int sx = ux ^ ax;
  int ex = ax >> EXPSHIFTBITS_SP32;

  int uy = __clc_as_int(y);
  int ay = uy & EXSIGNBIT_SP32;
  float ya = __clc_as_float(ay);
  int sy = uy ^ ay;
  int ey = ay >> EXPSHIFTBITS_SP32;

  float xr = __clc_as_float(0x3f800000 | (ax & 0x007fffff));
  float yr = __clc_as_float(0x3f800000 | (ay & 0x007fffff));
  int c;
  int k = ex - ey;

  uint q = 0;

  while (k > 0) {
    c = xr >= yr;
    q = (q << 1) | c;
    xr -= c ? yr : 0.0f;
    xr += xr;
    --k;
  }

  c = xr > yr;
  q = (q << 1) | c;
  xr -= c ? yr : 0.0f;

  int lt = ex < ey;

  q = lt ? 0 : q;
  xr = lt ? xa : xr;
  yr = lt ? ya : yr;

  c = (yr < 2.0f * xr) | ((yr == 2.0f * xr) & ((q & 0x1) == 0x1));
  xr -= c ? yr : 0.0f;
  q += c;

  float s = __clc_as_float(ey << EXPSHIFTBITS_SP32);
  xr *= lt ? 1.0f : s;

  int qsgn = sx == sy ? 1 : -1;
  int quot = (q & 0x7f) * qsgn;

  c = ax == ay;
  quot = c ? qsgn : quot;
  xr = c ? 0.0f : xr;

  xr = __clc_as_float(sx ^ __clc_as_int(xr));

  c = ax > PINFBITPATT_SP32 | ay > PINFBITPATT_SP32 | ax == PINFBITPATT_SP32 |
      ay == 0;
  quot = c ? 0 : quot;
  xr = c ? __clc_as_float(QNANBITPATT_SP32) : xr;

  *quo = quot;

  return xr;
}

// remquo signature is special, we don't have macro for this
#define __VEC_REMQUO(TYPE, VEC_SIZE, HALF_VEC_SIZE)                            \
  _CLC_DEF _CLC_OVERLOAD TYPE##VEC_SIZE __clc_remquo(                          \
      TYPE##VEC_SIZE x, TYPE##VEC_SIZE y,                                      \
      __CLC_ADDRESS_SPACE int##VEC_SIZE *quo) {                                \
    int##HALF_VEC_SIZE lo, hi;                                                 \
    TYPE##VEC_SIZE ret;                                                        \
    ret.lo = __clc_remquo(x.lo, y.lo, &lo);                                    \
    ret.hi = __clc_remquo(x.hi, y.hi, &hi);                                    \
    (*quo).lo = lo;                                                            \
    (*quo).hi = hi;                                                            \
    return ret;                                                                \
  }

#define __VEC3_REMQUO(TYPE)                                                    \
  _CLC_DEF _CLC_OVERLOAD TYPE##3 __clc_remquo(                                 \
      TYPE##3 x, TYPE##3 y, __CLC_ADDRESS_SPACE int##3 * quo) {                \
    int2 lo;                                                                   \
    int hi;                                                                    \
    TYPE##3 ret;                                                               \
    ret.s01 = __clc_remquo(x.s01, y.s01, &lo);                                 \
    ret.s2 = __clc_remquo(x.s2, y.s2, &hi);                                    \
    (*quo).s01 = lo;                                                           \
    (*quo).s2 = hi;                                                            \
    return ret;                                                                \
  }
__VEC_REMQUO(float, 2, )
__VEC3_REMQUO(float)
__VEC_REMQUO(float, 4, 2)
__VEC_REMQUO(float, 8, 4)
__VEC_REMQUO(float, 16, 8)

#ifdef cl_khr_fp64

#pragma OPENCL EXTENSION cl_khr_fp64 : enable

_CLC_DEF _CLC_OVERLOAD double __clc_remquo(double x, double y,
                                           __CLC_ADDRESS_SPACE int *pquo) {
  ulong ux = __clc_as_ulong(x);
  ulong ax = ux & ~SIGNBIT_DP64;
  ulong xsgn = ux ^ ax;
  double dx = __clc_as_double(ax);
  int xexp = __clc_convert_int(ax >> EXPSHIFTBITS_DP64);
  int xexp1 = 11 - (int)__clc_clz(ax & MANTBITS_DP64);
  xexp1 = xexp < 1 ? xexp1 : xexp;

  ulong uy = __clc_as_ulong(y);
  ulong ay = uy & ~SIGNBIT_DP64;
  double dy = __clc_as_double(ay);
  int yexp = __clc_convert_int(ay >> EXPSHIFTBITS_DP64);
  int yexp1 = 11 - (int)__clc_clz(ay & MANTBITS_DP64);
  yexp1 = yexp < 1 ? yexp1 : yexp;

  int qsgn = ((ux ^ uy) & SIGNBIT_DP64) == 0UL ? 1 : -1;

  // First assume |x| > |y|

  // Set ntimes to the number of times we need to do a
  // partial remainder. If the exponent of x is an exact multiple
  // of 53 larger than the exponent of y, and the mantissa of x is
  // less than the mantissa of y, ntimes will be one too large
  // but it doesn't matter - it just means that we'll go round
  // the loop below one extra time.
  int ntimes = __clc_max(0, (xexp1 - yexp1) / 53);
  double w = __clc_ldexp(dy, ntimes * 53);
  w = ntimes == 0 ? dy : w;
  double scale = ntimes == 0 ? 1.0 : 0x1.0p-53;

  // Each time round the loop we compute a partial remainder.
  // This is done by subtracting a large multiple of w
  // from x each time, where w is a scaled up version of y.
  // The subtraction must be performed exactly in quad
  // precision, though the result at each stage can
  // fit exactly in a double precision number.
  int i;
  double t, v, p, pp;

  for (i = 0; i < ntimes; i++) {
    // Compute integral multiplier
    t = __clc_trunc(dx / w);

    // Compute w * t in quad precision
    p = w * t;
    pp = __clc_fma(w, t, -p);

    // Subtract w * t from dx
    v = dx - p;
    dx = v + (((dx - v) - p) - pp);

    // If t was one too large, dx will be negative. Add back one w.
    dx += dx < 0.0 ? w : 0.0;

    // Scale w down by 2^(-53) for the next iteration
    w *= scale;
  }

  // One more time
  // Variable todd says whether the integer t is odd or not
  t = __clc_floor(dx / w);
  long lt = (long)t;
  int todd = lt & 1;

  p = w * t;
  pp = __clc_fma(w, t, -p);
  v = dx - p;
  dx = v + (((dx - v) - p) - pp);
  i = dx < 0.0;
  todd ^= i;
  dx += i ? w : 0.0;

  lt -= i;

  // At this point, dx lies in the range [0,dy)

  // For the remainder function, we need to adjust dx
  // so that it lies in the range (-y/2, y/2] by carefully
  // subtracting w (== dy == y) if necessary. The rigmarole
  // with todd is to get the correct sign of the result
  // when x/y lies exactly half way between two integers,
  // when we need to choose the even integer.

  int al = (2.0 * dx > w) | (todd & (2.0 * dx == w));
  double dxl = dx - (al ? w : 0.0);

  int ag = (dx > 0.5 * w) | (todd & (dx == 0.5 * w));
  double dxg = dx - (ag ? w : 0.0);

  dx = dy < 0x1.0p+1022 ? dxl : dxg;
  lt += dy < 0x1.0p+1022 ? al : ag;
  int quo = ((int)lt & 0x7f) * qsgn;

  double ret = __clc_as_double(xsgn ^ __clc_as_ulong(dx));
  dx = __clc_as_double(ax);

  // Now handle |x| == |y|
  int c = dx == dy;
  t = __clc_as_double(xsgn);
  quo = c ? qsgn : quo;
  ret = c ? t : ret;

  // Next, handle |x| < |y|
  c = dx < dy;
  quo = c ? 0 : quo;
  ret = c ? x : ret;

  c &= (yexp < 1023 & 2.0 * dx > dy) | (dx > 0.5 * dy);
  quo = c ? qsgn : quo;
  // we could use a conversion here instead since qsgn = +-1
  p = qsgn == 1 ? -1.0 : 1.0;
  t = __clc_fma(y, p, x);
  ret = c ? t : ret;

  // We don't need anything special for |x| == 0

  // |y| is 0
  c = dy == 0.0;
  quo = c ? 0 : quo;
  ret = c ? __clc_as_double(QNANBITPATT_DP64) : ret;

  // y is +-Inf, NaN
  c = yexp > BIASEDEMAX_DP64;
  quo = c ? 0 : quo;
  t = y == y ? x : y;
  ret = c ? t : ret;

  // x is +=Inf, NaN
  c = xexp > BIASEDEMAX_DP64;
  quo = c ? 0 : quo;
  ret = c ? __clc_as_double(QNANBITPATT_DP64) : ret;

  *pquo = quo;
  return ret;
}
__VEC_REMQUO(double, 2, )
__VEC3_REMQUO(double)
__VEC_REMQUO(double, 4, 2)
__VEC_REMQUO(double, 8, 4)
__VEC_REMQUO(double, 16, 8)

#endif

#ifdef cl_khr_fp16

#pragma OPENCL EXTENSION cl_khr_fp16 : enable

_CLC_OVERLOAD _CLC_DEF half __clc_remquo(half x, half y,
                                         __CLC_ADDRESS_SPACE int *pquo) {
  return (half)__clc_remquo((float)x, (float)y, pquo);
}
__VEC_REMQUO(half, 2, )
__VEC3_REMQUO(half)
__VEC_REMQUO(half, 4, 2)
__VEC_REMQUO(half, 8, 4)
__VEC_REMQUO(half, 16, 8)

#endif
