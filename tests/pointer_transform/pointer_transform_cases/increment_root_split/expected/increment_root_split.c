#include <stdio.h>

/* `q = p++` is an ordinary (base, index) assignment: the base is `p` and the
 * position `q` lands at is p's index *and* the step, so it pairs as
 * `int *q = p; int q_index_xj = p_index_xj++;`. Keeping both pointers in
 * split form is what lets `q` be reconstructed against `p` later; materializing
 * `p + p_index_xj++` into `q` instead would give `q` a derived interior base,
 * and one containing a side effect at that.
 *
 * The step cannot ride in the offset terms — it both reads and mutates the
 * base's index — so it is carried beside them. Pre- and post- forms differ
 * only in where the operator lands, which is exactly the distinction the
 * index has to reproduce: post- hands `q` the old position, pre- the new one.
 *
 * The `+ 1` case is the reason the decomposition recurses rather than walking
 * a spine: `p++ + 1` is the increment case with an addend on top, and neither
 * needs to know about the other. */

/* Post-increment: q takes the old position, p advances. */
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

/* Pre-increment: q takes the new position. */
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

/* Post-decrement walking backwards. */
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

/* The step with an addend on top: `q_index_xj = p_index_xj++ + 1`. */
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
