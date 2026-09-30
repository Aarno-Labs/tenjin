#include <stdio.h>

static int sum_range(int *lo, int *hi) {
    int total = 0;
    int p_index_xj = 0;
    while (p_index_xj < (hi - lo)) {
        total += lo[p_index_xj];
        p_index_xj++;
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
