/* Same basename as src2/util.c, same static name, different lookback:
 * this one reads *(p - 1), so its slice is widened below the base. */

#include <stddef.h>

static int scale(const int *buf, int n) {
    int p_index_xj = 1;
    int s = 0;
    while (p_index_xj < n) {
        s += buf[p_index_xj - 1] + buf[p_index_xj];
        p_index_xj++;
    }
    return s;
}

int src1_scale(const int *buf, int n) { return scale(buf, n); }
