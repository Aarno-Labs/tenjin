/* Same basename as src2/util.c, same static name, different lookback:
 * this one reads *(p - 1), so its slice is widened below the base. */

#include <stddef.h>

typedef struct { int *ptr; size_t len; } RustSlice_int;

static int scale(RustSlice_int arr) {
    int p_index_xj = 1 + 1;
    int s = 0;
    while (p_index_xj < arr.len) {
        s += arr.ptr[p_index_xj - 1] + arr.ptr[p_index_xj];
        p_index_xj++;
    }
    return s;
}

int src1_scale(const int *buf, int n) { return scale((RustSlice_int){buf - 1, n + 1}); }
