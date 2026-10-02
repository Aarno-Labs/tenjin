#include <stdio.h>

/* q is initialized from an increment or decrement of p. */

typedef struct { int *ptr; size_t len; } RustSlice_int;

static int post(RustSlice_int arr) {
    int p_index_xj = 0;
    int s = 0;
    for (int i = 0; i < arr.len / 2; i++) {
        s += arr.ptr[p_index_xj];
        p_index_xj++;
    }
    int q_index_xj = p_index_xj++;
    while (q_index_xj < arr.len) {
        s += arr.ptr[q_index_xj] * 2;
        q_index_xj++;
    }
    return s;
}

static int pre(RustSlice_int arr) {
    int p_index_xj = 0;
    int s = 0;
    for (int i = 0; i < arr.len / 2; i++) {
        s += arr.ptr[p_index_xj];
        p_index_xj++;
    }
    int q_index_xj = ++p_index_xj;
    while (q_index_xj < arr.len) {
        s += arr.ptr[q_index_xj] * 3;
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

static int stepped_offset(RustSlice_int arr) {
    int p_index_xj = 0;
    int s = 0;
    for (int i = 0; i < arr.len / 2; i++) {
        s += arr.ptr[p_index_xj];
        p_index_xj++;
    }
    int q_index_xj = p_index_xj++ + 1;
    while (q_index_xj < arr.len) {
        s += arr.ptr[q_index_xj] * 7;
        q_index_xj++;
    }
    return s;
}

int main(void) {
    int v[6] = {1, 2, 4, 8, 16, 32};
    printf("%d %d %d %d\n", post((RustSlice_int){v, 6}), pre((RustSlice_int){v, 6}), post_dec(v, 6),
           stepped_offset((RustSlice_int){v, 6}));
    return 0;
}
