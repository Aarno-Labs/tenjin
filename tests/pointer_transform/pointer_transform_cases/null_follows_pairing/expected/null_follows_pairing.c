#include <assert.h>
#include <stdio.h>
#include <string.h>

/* `p = q` copies q's index, so a null that q's index can hold reaches p
 * with it, and p's null tests and value reads have to allow for it. */

/* A failed search: the sentinel sits over a region that is not null. */
static int strchr_index_xj(const char *base, int start, int c) {
    const char *result = strchr(base + start, c);
    if (!result) return -1;
    return (int)(result - base);
}

static int after_comma(char *s) {
    int q_index_xj = 0;
    q_index_xj = strchr_index_xj(s, q_index_xj, ',');
    int p_index_xj = q_index_xj;
    if (!(s && p_index_xj >= 0))
        return -1;
    p_index_xj++;
    return s[p_index_xj];
}

/* A plain NULL: the region is null too. */
static char *skip_then_next(char *s, int skip) {
    char *q = NULL;
    int q_index_xj = -1;
    if (s) {
        (q = s, q_index_xj = 0);
        q_index_xj += skip;
    }
    int p_index_xj = q_index_xj;
    if (q)
        p_index_xj++;
    return (q ? q + p_index_xj : (void *)0);
}

/* Copied twice. */
static int strstr_index_xj(const char *base, int start, const char *needle) {
    const char *result = strstr(base + start, needle);
    if (!result) return -1;
    return (int)(result - base);
}

static int after_colons(char *s) {
    int q_index_xj = 0;
    int p_index_xj = 0;
    int r_index_xj = 0;
    q_index_xj = strstr_index_xj(s, q_index_xj, "::");
    (p_index_xj = q_index_xj);
    if ((s && p_index_xj >= 0))
        p_index_xj += 2;
    (r_index_xj = p_index_xj);
    if ((!s || r_index_xj < 0))
        return -1;
    r_index_xj++;
    return s[r_index_xj];
}

/* A null test inside a macro cannot be rewritten, so a pointer that would
 * need it rewritten is left alone, and is handed q's value instead of
 * its index. */
static int asserted(char *s) {
    int q_index_xj = 0;
    q_index_xj = strchr_index_xj(s, q_index_xj, ',');
    char *p = (q_index_xj < 0 ? (void *)0 : s + q_index_xj);
    assert(p);
    p++;
    int r_index_xj = 0;
    r_index_xj++;
    return *p * 1000 + p[r_index_xj];
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
