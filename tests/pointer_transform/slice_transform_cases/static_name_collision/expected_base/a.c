/* The detectable half of each colliding pair. */

#include <stddef.h>

static int table[4] = {10, 20, 30, 40};

/* Slice-detectable: a moving pointer bounded by buf + n. */
static int sum(const int *buf, int n) {
    int p_index_xj = 0;
    int s = 0;
    while (p_index_xj < n) {
        s += buf[p_index_xj];
        p_index_xj++;
    }
    return s;
}

/* Global-return: every return is NULL or &table[i]. */
static int *pick(int i) {
    if (i >= 0 && i < 4)
        return &table[i];
    return (void *)0;
}

int a_sum(const int *buf, int n) { return sum(buf, n); }

int a_pick(int i) {
    int *p = pick(i);
    return p ? *p : -1;
}
