/* The detectable half of each colliding pair. */

#include <stddef.h>

static int table[4] = {10, 20, 30, 40};

/* Slice-detectable: a moving pointer bounded by buf + n. */
typedef struct { int *ptr; size_t len; } RustSlice_int;

static int sum(RustSlice_int arr) {
    int p_index_xj = 0;
    int s = 0;
    while (p_index_xj < arr.len) {
        s += arr.ptr[p_index_xj];
        p_index_xj++;
    }
    return s;
}

/* Global-return: every return is NULL or &table[i]. */
static int pick(int i) {
    if (i >= 0 && i < 4)
        return i;
    return -1;
}

int a_sum(const int *buf, int n) { return sum((RustSlice_int){buf, n}); }

int a_pick(int i) {
    int p = pick(i);
    return p ? table[p] : -1;
}
