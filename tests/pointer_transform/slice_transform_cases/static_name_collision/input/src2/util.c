/* Same basename as src1/util.c, same static name, no lookback: this
 * one must keep an un-widened slice. */

#include <stddef.h>

static int scale(const int *buf, int n) {
    const int *p = buf;
    int s = 0;
    while (p < buf + n) {
        s += *p * 2;
        p++;
    }
    return s;
}

int src2_scale(const int *buf, int n) { return scale(buf, n); }
