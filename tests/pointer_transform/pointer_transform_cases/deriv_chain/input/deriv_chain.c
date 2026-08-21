#include <stdio.h>

/* A chain of derived pointers, each starting where the previous one is.
 * `q = p + 1` is the ordinary assignment rule — pair the bases, add the
 * offsets — rather than a special inheritance fixup, so a chain of any
 * length composes on its own. */
static int chain(int *a, int n) {
    int *p = a;
    int *q = p + 1;
    int *r = q + 1;
    int s = 0;
    while (r - a < n) {
        s += *p + *q * 2 + *r * 4;
        p++;
        q++;
        r++;
    }
    return s;
}

int main(void) {
    int v[6] = {1, 2, 3, 4, 5, 6};
    printf("%d\n", chain(v, 6));
    return 0;
}
