/* Same basename as src2/util.c, same static name, different lookback:
 * this one reads *(p - 1), so its slice is widened below the base. */

#include <stddef.h>

static int scale(const int *buf, int n) {
    const int *p = buf + 1;
    int s = 0;
    while (p < buf + n) {
        s += *(p - 1) + *p;
        p++;
    }
    return s;
}

int src1_scale(const int *buf, int n) { return scale(buf, n); }
