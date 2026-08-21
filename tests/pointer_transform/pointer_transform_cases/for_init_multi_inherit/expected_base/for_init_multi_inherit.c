#include <stdio.h>

/* A for-init declarator initialized from its own neighbour. `q` inherits
 * `r`'s index, so its index initializer would read `r_index_xj + (1)` —
 * a name bound by the very statement the index has to be hoisted out of.
 * `q` is frozen instead, which is what keeps the remaining hoists
 * independent of each other: no ordering between them is required.
 *
 * `r` still collapses. Its declaration survives (the other declarator
 * needs it), so `q`'s initializer keeps reading `r + 1` verbatim — still
 * correct, because `r` is freshly initialized at that point and never
 * advances again. */
static int derived(int *a, int n) {
    int s = 0;
    int r_index_xj = 0;
    int q_index_xj = r_index_xj + 1;
    for (int *r = a, *q = r; n > 1; n--, r_index_xj++, q_index_xj++)
        s += r[r_index_xj] * 10 + q[q_index_xj];
    return s;
}

int main(void) {
    int a[6] = {1, 2, 3, 4, 5, 6};
    printf("%d\n", derived(a, 4));
    return 0;
}
