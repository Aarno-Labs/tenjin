#include <assert.h>
#include <stdio.h>
#include <string.h>

/* `p = q` copies q's index, so a null that q's index can hold reaches p
 * with it, and p's null tests and value reads have to allow for it. */

/* A failed search: the sentinel sits over a region that is not null. */
static int after_comma(char *s) {
    char *q = s;
    q = strchr(q, ',');
    char *p = q;
    if (!p)
        return -1;
    p++;
    return *p;
}

/* A plain NULL: the region is null too. */
static char *skip_then_next(char *s, int skip) {
    char *q = NULL;
    if (s) {
        q = s;
        q += skip;
    }
    char *p = q;
    if (p)
        p++;
    return p;
}

/* Copied twice. */
static int after_colons(char *s) {
    char *q = s;
    char *p;
    char *r;
    q = strstr(q, "::");
    p = q;
    if (p != NULL)
        p += 2;
    r = p;
    if (r == NULL)
        return -1;
    r++;
    return *r;
}

/* A null test inside a macro cannot be rewritten, so a pointer that would
 * need it rewritten is left alone, and is handed q's value instead of
 * its index. */
static int asserted(char *s) {
    char *q = s;
    q = strchr(q, ',');
    char *p = q;
    assert(p);
    p++;
    char *r = p;
    r++;
    return *p * 1000 + *r;
}

int main(void) {
    char a[] = "ab,c";
    char b[] = "abc";
    char c[] = "a::bc";
    printf("%d %d\n", after_comma(a), after_comma(b));
    char *r = skip_then_next(a, 2);
    printf("%s\n", r ? r : "(null)");
    r = skip_then_next(NULL, 2);
    printf("%s\n", r ? "non-null" : "(null)");
    printf("%d %d\n", after_colons(c), after_colons(b));
    printf("%d\n", asserted(a));
    return 0;
}
