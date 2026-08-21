#include <stdio.h>
#include <string.h>

/* The search need not read the pointer it lands in. The result is an offset
 * into whatever region argument 0 names, so that region is what the assigned
 * pointer has to end up holding.
 *
 * zmy expressed this by passing its separately tracked base to the wrapper —
 * the shape its own comment called out, `l = strchr(path, sep)` in Lua. With
 * the base and the pointer now one variable, the reseat carries it instead. */

/* The searched region is an ordinary pointer nobody rewrote. */
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

/* The searched region is itself rewritten, so the search starts from that
 * pointer's index rather than from its origin. */
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
