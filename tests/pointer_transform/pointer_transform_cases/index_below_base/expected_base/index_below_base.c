#include <stdio.h>
#include <stddef.h>

/* An index counts from wherever its pointer was last seated, so it goes
 * negative when the pointer steps back from there. */

/* A negative index is not the null sentinel. */
static int count_back(char *start, char *end) {
    char *p = NULL;
    int p_index_xj = -1;
    int n = 0;
    if (start)
        (p = end, p_index_xj = 0);
    while ((p ? p + p_index_xj : (void *)0) > start) {
        p_index_xj--;
        if (p[p_index_xj] == 'x') n++;
    }
    return n;
}

/* An unsigned offset must not make the subscript unsigned. */
static int back(const char *end, unsigned n) {
    int p_index_xj = 0;
    int s = 0;
    s += end[p_index_xj - (int)n];
    p_index_xj--;
    s += end[p_index_xj];
    return s;
}

static int back_subscript(const char *end, unsigned i, size_t n) {
    int p_index_xj = 0;
    p_index_xj -= 5;
    int s = end[p_index_xj + (int)i] * 10000;
    s += end[p_index_xj + (long)n] * 100;
    p_index_xj -= 1;
    s += end[p_index_xj + (int)(i + 1u)];
    return s;
}

int main(void) {
    char t[] = "axbxc";
    const char *u = "abcdefgh";
    printf("%d\n", count_back(t, t + 5));
    printf("%d\n", back(u + 4, 2u));
    printf("%d\n", back_subscript(u + 6, 1u, 2));
    return 0;
}
