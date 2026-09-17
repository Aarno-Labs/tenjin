#include <math.h>

static int left_calls;
static int right_calls;

static float counted_left(float value) {
    ++left_calls;
    return value;
}

static double counted_right(double value) {
    ++right_calls;
    return value;
}

int main(void) {
    float normal = 1.0f;
    float subnormal = 0x1p-149f;
    float negative_zero = -0.0f;

    if (!isnormal(normal)) {
        return 1;
    }
    if (isnormal(subnormal)) {
        return 2;
    }
    if (isnormal(INFINITY)) {
        return 3;
    }
    if (!signbit(negative_zero)) {
        return 4;
    }
    if (signbit(normal)) {
        return 5;
    }
    if (!isfinite(normal)) {
        return 6;
    }
    if (isfinite(INFINITY)) {
        return 7;
    }
    if (!isgreater(2.0f, 1.0)) {
        return 8;
    }
    if (!isgreaterequal(2.0f, 2.0)) {
        return 9;
    }
    if (!isless(1.0f, 2.0)) {
        return 10;
    }
    if (!islessequal(2.0f, 2.0)) {
        return 11;
    }
    if (!islessgreater(1.0f, 2.0)) {
        return 12;
    }
    if (islessgreater(NAN, 2.0)) {
        return 13;
    }
    if (!isunordered(NAN, 2.0)) {
        return 14;
    }
    if (isunordered(1.0f, 2.0)) {
        return 15;
    }
    left_calls = right_calls = 0;
    if (!islessgreater(counted_left(1.0f), counted_right(2.0)) ||
        left_calls != 1 || right_calls != 1) {
        return 16;
    }
    left_calls = right_calls = 0;
    if (!isunordered(counted_left(NAN), counted_right(2.0)) ||
        left_calls != 1 || right_calls != 1) {
        return 17;
    }
    if (fpclassify(NAN) != FP_NAN) {
        return 18;
    }
    if (fpclassify(INFINITY) != FP_INFINITE) {
        return 19;
    }
    if (fpclassify(normal) != FP_NORMAL) {
        return 20;
    }
    if (fpclassify(subnormal) != FP_SUBNORMAL) {
        return 21;
    }
    if (fpclassify(negative_zero) != FP_ZERO) {
        return 22;
    }
    left_calls = 0;
    if (fpclassify(counted_left(normal)) != FP_NORMAL || left_calls != 1) {
        return 23;
    }
    return 0;
}
