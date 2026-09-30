#include <stdio.h>

/* A for-init declarator whose initializer names a non-pointer bound by
 * the same statement. Collapsing `p` would put `int p_index_xj = i;`
 * before the loop, where `i` does not exist yet, so `p` is frozen and
 * carries an index of 0 alongside the pointer instead. */
static int offset_by_sibling(int *a, int k, int n) {
    int s = 0;
    for (int i = k, *p = a + i; n > 0; n--, i++, p++)
        s += *p + i;
    return s;
}

int main(void) {
    int a[8] = {1, 2, 4, 8, 16, 32, 64, 128};
    printf("%d\n", offset_by_sibling(a, 2, 3));
    return 0;
}
