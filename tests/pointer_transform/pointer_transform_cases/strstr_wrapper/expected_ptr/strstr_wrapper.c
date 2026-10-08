#include <stdio.h>
#include <string.h>

/* The value of a strstr assignment is used in a condition. */
static int strstr_index_xj(const char *base, int start, const char *needle) {
    const char *result = strstr(base + start, needle);
    if (!result) return -1;
    return (int)(result - base);
}

static int count_needles(const char *hay, const char *needle) {
    const char *p = hay;
    int p_index_xj = 0;
    int n = 0;

    while (((p_index_xj = strstr_index_xj(p, p_index_xj, needle), (p_index_xj < 0 ? (void *)0 : p + p_index_xj))) != NULL) {
        n++;
        p_index_xj++;
    }
    return n;
}

int main(void) {
    printf("%d\n", count_needles("abababa", "aba"));
    printf("%d\n", count_needles("xyz", "aba"));
    return 0;
}
