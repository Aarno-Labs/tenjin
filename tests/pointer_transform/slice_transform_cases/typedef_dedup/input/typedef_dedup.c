#include <stdio.h>

/* Two int-slice functions must share one RustSlice_int typedef; the char
 * function gets its own RustSlice_char typedef. */
static int sum_ints(int *buf, int n) {
    int *p = buf;
    int s = 0;
    while (p < buf + n) {
        s += *p++;
    }
    return s;
}

static int max_int(int *buf, int n) {
    int *p = buf;
    int best = -1000000;
    while (p < buf + n) {
        if (*p > best)
            best = *p;
        p++;
    }
    return best;
}

static int count_upper(char *buf, int n) {
    char *p = buf;
    int count = 0;
    while (p < buf + n) {
        if (*p >= 'A' && *p <= 'Z')
            count++;
        p++;
    }
    return count;
}

int main(void) {
    int d[4] = {4, 8, 15, 16};
    char s[5] = {'a', 'B', 'c', 'D', 'e'};
    printf("%d %d %d\n", sum_ints(d, 4), max_int(d, 4), count_upper(s, 5));
    return 0;
}
