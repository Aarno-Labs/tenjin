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

static void first_comma(const char *s) {
    const char *p = s;

    p = strchr(p, ',');
    observe(p, "comma");
}

static void pick(const char *s, int take) {
    const char *p;

    if (take)
        p = s + 1;
    else
        p = NULL;
    observe(p, "pick");
}

/* The same sentinel crossing a return boundary rather than a call. */
static const char *maybe_tail(const char *s, int take) {
    const char *p;

    if (take)
        p = s + 2;
    else
        p = NULL;
    return p;
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
