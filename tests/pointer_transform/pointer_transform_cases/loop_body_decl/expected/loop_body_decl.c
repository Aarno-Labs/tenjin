#include <stdio.h>

/* `p` is declared inside a loop body, so it is re-initialized on every
 * iteration and its index has to restart at 0 alongside it. Pins the
 * placement rule: the index declaration must execute exactly as often as
 * the declaration it accompanies, which rules out hoisting it out of the
 * loop. */
static int f(int *a, int *b, int n) {
    int s = 0;
    for (int i = 0; i < n; i++) {
        int p_index_xj = 0;
        int *p = a;
        if (i == 1)
            (p = b, p_index_xj = 0);
        p_index_xj++;
        s += p[p_index_xj];
    }
    return s;
}

int main(void) {
    int a[4] = {1, 2, 4, 8};
    int b[4] = {10, 20, 40, 80};
    printf("%d\n", f(a, b, 3));
    return 0;
}
