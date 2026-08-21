#include <stdio.h>
#include <string.h>

/* A NULL-able pointer is represented by the out-of-range index -1, so
 * every site that turns an index back into a pointer has to map that
 * sentinel back to a real null: `base + -1` addresses one element
 * before the array and is *not* null, so a callee's null check would
 * wrongly succeed.
 *
 * The two ways an index goes negative are covered here:
 *   - an explicit NULL assignment (`pick`), and
 *   - an allowlisted-function wrapper returning -1 for "not found"
 *     (`first_comma`), where no NULL appears in the source at all.
 *
 * `observe` is deliberately left untransformed, so it sees whatever
 * pointer the rewrite actually produces. */
static void observe(const char *q, const char *tag) {
    printf("%s=%s\n", tag, q ? "nonnull" : "null");
}

static int strchr_index_xj(const char *base, int start, int c) {
    const char *result = strchr(base + start, c);
    if (!result) return -1;
    return (int)(result - base);
}

static void first_comma(const char *s) {
    const char *p = s;
    int p_index_xj = 0;

    p_index_xj = strchr_index_xj(p, p_index_xj, ',');
    observe((p_index_xj < 0 ? (void *)0 : p + p_index_xj), "comma");
}

static void pick(const char *s, int take) {
    const char *p;
    int p_index_xj = 0;

    if (take)
        (p = s, p_index_xj = 1);
    else
        (p = NULL, p_index_xj = -1);
    observe((p_index_xj < 0 ? (void *)0 : p + p_index_xj), "pick");
}

/* The same sentinel crossing a return boundary rather than a call. */
static const char *maybe_tail(const char *s, int take) {
    const char *p;
    int p_index_xj = 0;

    if (take)
        (p = s, p_index_xj = 2);
    else
        (p = NULL, p_index_xj = -1);
    return (p_index_xj < 0 ? (void *)0 : p + p_index_xj);
}

int main(void) {
    first_comma("a,b");
    first_comma("nothing here");
    pick("abc", 1);
    pick("abc", 0);
    observe(maybe_tail("abcdef", 1), "tail1");
    observe(maybe_tail("abcdef", 0), "tail0");
    return 0;
}
