#include <stdio.h>

/* `*(p + stride)` has a non-constant offset. That used to disqualify the
 * pointer entirely; now it transforms (the base here is stable, so via
 * base collapse) and merely fails to qualify as a slice, since no sound
 * static bound exists. */
static int gather(const int *buf, int n, int stride) {
    int p_index_xj = 0;
    int s = 0;
    for (int i = 0; i + stride < n; i++) {
        s += buf[p_index_xj + stride];
        p_index_xj++;
    }
    return s;
}

int main(void) {
    int d[6] = {1, 2, 4, 8, 16, 32};
    printf("%d %d\n", gather(d, 6, 2), gather(d, 6, 3));
    return 0;
}
