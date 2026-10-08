#include <stdio.h>
#include <string.h>

/* strchr searches a different pointer than the one it is assigned to. */

/* The searched pointer is not rewritten. */
static int strchr_index_xj(const char *base, int start, int c) {
    const char *result = strchr(base + start, c);
    if (!result) return -1;
    return (int)(result - base);
}

static int after_sep(const char *path, int sep) {
    int l_index_xj = 0;
    int n = 0;

    (l_index_xj = strchr_index_xj(path, 0, sep));
    if (!(path && l_index_xj >= 0))
        return -1;
    l_index_xj++;
    while (path[l_index_xj]) {
        n++;
        l_index_xj++;
    }
    return n;
}

/* The searched pointer is rewritten. */
static int after_sep_moved(const char *s, int sep) {
    int q_index_xj = 0;
    int l_index_xj = 0;
    int n = 0;

    q_index_xj++;
    (l_index_xj = strchr_index_xj(s, q_index_xj, sep));
    if (!(s && l_index_xj >= 0))
        return -1;
    l_index_xj++;
    while (s[l_index_xj]) {
        n++;
        l_index_xj++;
    }
    return n;
}

int main(void) {
    printf("%d\n", after_sep("ab/cdef", '/'));
    printf("%d\n", after_sep("nosep", '/'));
    printf("%d\n", after_sep_moved("/ab/cdef", '/'));
    printf("%d\n", after_sep_moved("/abcdef", '/'));
    return 0;
}
