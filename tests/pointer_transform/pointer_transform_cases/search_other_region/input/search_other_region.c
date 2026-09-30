#include <stdio.h>
#include <string.h>

/* strchr searches a different pointer than the one it is assigned to. */

/* The searched pointer is not rewritten. */
static int after_sep(const char *path, int sep) {
    const char *l = path;
    int n = 0;

    l = strchr(path, sep);
    if (!l)
        return -1;
    l++;
    while (*l) {
        n++;
        l++;
    }
    return n;
}

/* The searched pointer is rewritten. */
static int after_sep_moved(const char *s, int sep) {
    const char *q = s;
    const char *l = s;
    int n = 0;

    q++;
    l = strchr(q, sep);
    if (!l)
        return -1;
    l++;
    while (*l) {
        n++;
        l++;
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
