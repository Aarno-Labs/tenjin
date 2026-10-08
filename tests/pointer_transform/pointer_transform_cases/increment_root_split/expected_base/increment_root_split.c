#include <stdio.h>

/* q is initialized from an increment or decrement of p. */

static int post(int *a, int n) {
    int p_index_xj = 0;
    int s = 0;
    for (int i = 0; i < n / 2; i++) {
        s += a[p_index_xj];
        p_index_xj++;
    }
    int q_index_xj = p_index_xj++;
    while (q_index_xj < n) {
        s += a[q_index_xj] * 2;
        q_index_xj++;
    }
    return s;
}

static int pre(int *a, int n) {
    int p_index_xj = 0;
    int s = 0;
    for (int i = 0; i < n / 2; i++) {
        s += a[p_index_xj];
        p_index_xj++;
    }
    int q_index_xj = ++p_index_xj;
    while (q_index_xj < n) {
        s += a[q_index_xj] * 3;
        q_index_xj++;
    }
    return s;
}

static int post_dec(int *a, int n) {
    int p_index_xj = n - 1;
    int s = 0;
    for (int i = 0; i < n / 2; i++) {
        s += a[p_index_xj];
        p_index_xj--;
    }
    int q_index_xj = p_index_xj--;
    while (q_index_xj >= 0) {
        s += a[q_index_xj] * 5;
        q_index_xj--;
    }
    return s;
}

static int stepped_offset(int *a, int n) {
    int p_index_xj = 0;
    int s = 0;
    for (int i = 0; i < n / 2; i++) {
        s += a[p_index_xj];
        p_index_xj++;
    }
    int q_index_xj = p_index_xj++ + 1;
    while (q_index_xj < n) {
        s += a[q_index_xj] * 7;
        q_index_xj++;
    }
    return s;
}

int main(void) {
    int v[6] = {1, 2, 4, 8, 16, 32};
    printf("%d %d %d %d\n", post(v, 6), pre(v, 6), post_dec(v, 6),
           stepped_offset(v, 6));
    return 0;
}
