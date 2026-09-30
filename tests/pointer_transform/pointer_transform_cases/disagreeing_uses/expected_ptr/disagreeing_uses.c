#include <stdio.h>

/* A pointer with two possible bases is not reconstructed. */

static int crossing(int *a, int *b, int n) {
    int *p = a;
    int p_index_xj = 0;
    int s = 0;
    for (int i = 0; i < n; i++)
        s += p[p_index_xj++];
    p = b, p_index_xj = 0;
    for (int i = 0; i < n; i++)
        s += p[p_index_xj++] * 2;
    return s;
}

static int either(int *a, int *b, int flag, int n) {
    int *p = a;
    int p_index_xj = 0;
    if (flag)
        p = b, p_index_xj = 0;
    int s = 0;
    for (int i = 0; i < n; i++)
        s += p[p_index_xj++];
    return s;
}

int main(void) {
    int x[3] = {1, 2, 3};
    int y[3] = {10, 20, 30};
    printf("%d %d %d\n", crossing(x, y, 3), either(x, y, 0, 3), either(x, y, 1, 3));
    return 0;
}
