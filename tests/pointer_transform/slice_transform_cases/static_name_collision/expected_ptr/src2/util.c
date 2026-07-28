/* Same basename as src1/util.c, same static name, no lookback: this
 * one must keep an un-widened slice. */

#include <stddef.h>

static int scale(const int *buf, int n) {
    int p_index_xj = 0;
    int s = 0;
    while (p_index_xj < n) {
        s += buf[p_index_xj] * 2;
        p_index_xj++;
    }
    return s;
}

int src2_scale(const int *buf, int n) { return scale(buf, n); }
