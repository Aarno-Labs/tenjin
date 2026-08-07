#include <stdio.h>

/* `q` is derived from `p`, which is frozen because it is reseated. A
 * frozen pointer's value is its base, so `p + 1` has to be materialized
 * as `(p + p_index_xj) + 1` — otherwise q lands where p started rather
 * than where it had reached. Both pointers should transform. */
static int f(int *a, int *b, int n) {
    int *p = a;
    if (n < 0)
        p = b;
    p++;
    int *q = p + 1;
    return *p + *q;
}

int main(void) {
    int a[4] = {1, 2, 4, 8};
    int b[4] = {10, 20, 40, 80};
    printf("%d %d\n", f(a, b, 1), f(a, b, -1));
    return 0;
}
