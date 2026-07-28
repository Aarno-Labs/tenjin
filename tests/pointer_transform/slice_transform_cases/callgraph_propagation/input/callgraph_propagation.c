#include <stdio.h>

static int sum_range(int *lo, int *hi) {
    int total = 0;
    int *p = lo;
    while (p < hi) {
        total += *p;
        p++;
    }
    return total;
}

static int sum_all(int *lo, int *hi) {
    return sum_range(lo, hi);
}

int main(void) {
    int d[5] = {1, 2, 3, 4, 5};
    printf("%d\n", sum_all(d, d + 5));
    return 0;
}
