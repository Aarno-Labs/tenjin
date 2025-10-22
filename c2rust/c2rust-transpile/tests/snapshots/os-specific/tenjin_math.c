#include <math.h>
#include <stdio.h>
#include <float.h>
#include <fenv.h>

#pragma STDC FENV_ACCESS ON

static float test_basic(float x) {
    float y = 0.0f;
    y += fabsf(x);
    y += fmodf(x, 3.5f);
    y += remainderf(x, 2.0f);
    return y;
}

static float test_exp_log(float x) {
    float y = 0.0f;
    y += expf(x);
    y += exp2f(x);
    y += expm1f(x);
    y += logf(fabsf(x) + 1.0f);
    y += log10f(fabsf(x) + 1.0f);
    y += log1pf(fabsf(x));
    y += log2f(fabsf(x) + 1.0f);
    return y;
}

static float test_pow_root(float x) {
    float y = 0.0f;
    y += powf(x, 2.0f);
    y += sqrtf(fabsf(x));
    y += cbrtf(x);
    y += hypotf(x, 2.0f);
    return y;
}

static float test_trig(float x) {
    float y = 0.0f;
    y += sinf(x);
    y += cosf(x);
    y += tanf(x);
    y += asinf(fmaxf(fminf(x, 1.0f), -1.0f));
    y += acosf(fmaxf(fminf(x, 1.0f), -1.0f));
    y += atanf(x);
    y += atan2f(x, 1.0f);
    return y;
}

static float test_hyperbolic(float x) {
    float y = 0.0f;
    y += sinhf(x);
    y += coshf(x);
    y += tanhf(x);
    y += asinhf(x);
    y += acoshf(fabsf(x) + 1.0f);
    y += atanhf(fmaxf(fminf(x, 0.999f), -0.999f));
    return y;
}

static float test_nearby(float x) {
    float y = 0.0f;
    y += ceilf(x);
    y += floorf(x);
    y += truncf(x);
    y += roundf(x);
    y += nearbyintf(x);
    y += rintf(x);
    return y;
}

static float test_remainder_classify(float x) {
    float y = 0.0f;
    y += copysignf(x, -1.0f);
    y += nextafterf(x, 2.0f);
    //y += nexttowardf(x, 2.0); // avoid long double / f128 for now
    y += fdimf(x, 1.0f);
    y += fmaxf(x, 1.0f);
    y += fminf(x, 1.0f);
#ifndef __APPLE__
    if (isnan(x)) y += 1.0f;
    if (isinf(x)) y += 2.0f;
    if (isfinite(x)) y += 3.0f;
    if (signbit(x)) y += 4.0f;
    if (isnormal(x)) y += 5.0f;
#endif
    return y;
}

static float test_modf_split(float x) {
    float ipart;
    float frac = modff(x, &ipart);
    return ipart + frac;
}

static float test_frexp_ldexp(float x) {
    int exp;
    float mant = frexpf(x, &exp);
    return ldexpf(mant, exp);
}

#if 0
// Not yet supported by c2rust
static float test_fpclassify_funcs(float x) {
    float y = 0.0f;
    if (fpclassify(x) == FP_ZERO) y += 1.0f;
    if (fpclassify(x) == FP_NORMAL) y += 2.0f;
    return y;
}
#endif

int main(void) {
    float x = 0.75f;
    float sum = 0.0f;
    sum += test_basic(x);
    sum += test_exp_log(x);
    sum += test_pow_root(x);
    sum += test_trig(x);
    sum += test_hyperbolic(x);
    sum += test_nearby(x);
    sum += test_remainder_classify(x);
    sum += test_modf_split(x);
    sum += test_frexp_ldexp(x);
    //sum += test_fpclassify_funcs(x);
    printf("math.h smoke test checksum: %.6f\n", sum);
    return 0;
}
