#include <stdio.h>

/* Exercises lookback (via *(p - 1)) and lookahead (via *(p + 1)): the
 * constructed slice at the call site must be widened on both sides. */
static int lookaround(const int *buf, int n) {
    const int *p = buf + 1;
    int i = 1;
    int total = 0;
    while (p < buf + n) {
        total += *(p - 1);
        if (i + 1 < n)
            total += *(p + 1);
        i++;
        p++;
    }
    return total;
}

int main(void) {
    int d[5] = {1, 2, 3, 4, 5};
    printf("%d\n", lookaround(d, 5));
    return 0;
}
