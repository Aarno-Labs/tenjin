#include <math.h>

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
    return 0;
}
