/* Same basename as src1/util.c, same static name, no lookback: this
 * one must keep an un-widened slice. */

#include <stddef.h>

typedef struct { int *ptr; size_t len; } RustSlice_int;

static int scale(RustSlice_int arr) {
    int p_index_xj = 0;
    int s = 0;
    while (p_index_xj < arr.len) {
        s += arr.ptr[p_index_xj] * 2;
        p_index_xj++;
    }
    return s;
}

int src2_scale(const int *buf, int n) { return scale((RustSlice_int){buf, n}); }
