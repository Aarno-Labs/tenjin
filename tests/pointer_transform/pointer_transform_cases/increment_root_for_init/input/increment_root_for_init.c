#include <stdio.h>

/* q = p++ with both pointers declared in the same for-init. */

static int paired(int *a, int n) {
    int s = 0;
    for (int *p = a, *q = p++; p - a < n; p++, q++)
        s += *p * 2 + *q;
    return s;
}

/* The same with a constant offset. */
static int paired_second(int *a, int n) {
    int s = 0;
    for (int *p = a, *q = p + 1; p - a < n; p++, q++)
        s += *p + *q * 3;
    return s;
}

int main(void) {
    int v[8] = {1, 2, 4, 8, 16, 32, 64, 128};
    printf("%d %d\n", paired(v, 4), paired_second(v, 4));
    return 0;
}
