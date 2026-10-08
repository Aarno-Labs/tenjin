#include <stdio.h>

/* Two int-slice functions must share one RustSlice_int typedef; the char
 * function gets its own RustSlice_char typedef. */
static int sum_ints(int *buf, int n) {
    int p_index_xj = 0;
    int s = 0;
    while (p_index_xj < n) {
        s += buf[p_index_xj++];
    }
    return s;
}

static int max_int(int *buf, int n) {
    int p_index_xj = 0;
    int best = -1000000;
    while (p_index_xj < n) {
        if (buf[p_index_xj] > best)
            best = buf[p_index_xj];
        p_index_xj++;
    }
    return best;
}

static int count_upper(char *buf, int n) {
    int p_index_xj = 0;
    int count = 0;
    while (p_index_xj < n) {
        if (buf[p_index_xj] >= 'A' && buf[p_index_xj] <= 'Z')
            count++;
        p_index_xj++;
    }
    return count;
}

int main(void) {
    int d[4] = {4, 8, 15, 16};
    char s[5] = {'a', 'B', 'c', 'D', 'e'};
    printf("%d %d %d\n", sum_ints(d, 4), max_int(d, 4), count_upper(s, 5));
    return 0;
}
