#include <stdio.h>
#include <stddef.h>

/* An index counts from wherever its pointer was last seated, so it goes
 * negative when the pointer steps back from there. */

/* A negative index is not the null sentinel. */
static int count_back(char *start, char *end) {
    char *p = NULL;
    int n = 0;
    if (start)
        p = end;
    while (p > start) {
        p--;
        if (*p == 'x') n++;
    }
    return n;
}

/* An unsigned offset must not make the subscript unsigned. */
static int back(const char *end, unsigned n) {
    const char *p = end;
    int s = 0;
    s += *(p - n);
    p--;
    s += *p;
    return s;
}

static int back_subscript(const char *end, unsigned i, size_t n) {
    const char *p = end;
    p -= 5;
    int s = p[i] * 10000;
    s += *(p + n) * 100;
    p -= 1;
    s += p[i + 1u];
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
