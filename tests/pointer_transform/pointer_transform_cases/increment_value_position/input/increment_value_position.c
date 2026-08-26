#include <stdio.h>

/* An increment whose value is consumed has to hand back a pointer, not the
 * index it just bumped. These are the shapes with nowhere to put a paired
 * index — a comparison, a call argument, a cast, a pointer difference. None
 * of them assigns to a pointer this pass tracks, so each increment has to
 * materialize `base + index++` on its own.
 *
 * What this pins is how that decision is made. Listing the contexts that
 * consume a value leaves the list one short: a declarator was missing from
 * it, and `char *q = p++;` came out as `char *q = p_index_xj++;` — an int
 * where a pointer belongs, which is a hard error. The same omission put a
 * bare index into a comparison and a cast, where it compiles and quietly
 * compares an index against an address. Asking instead which positions
 * *discard* a value is a closed question, and the discarded positions are
 * pinned by `for_init_*` and by the plain `p++;` statements below. */

static int take(const char *q) { return *q; }

/* A comparison: `p++ < end` has to compare two pointers. */
static int count_below(char *p, char *end) {
    int n = 0;
    while (p++ < end)
        n += 1;
    return n;
}

/* A call argument. */
static int sum_via_call(char *p) {
    int n = 0;
    while (*p)
        n += take(p++);
    return n;
}

/* A cast. The cast is not stepped through, so the operand stays a value
 * read and the increment must still produce a pointer to cast. */
static int cast_escapes(char *p) {
    void *v = (void *)p++;
    while (*p)
        p++;
    return v != 0;
}

/* A pointer difference: the result is an integer, so there is no pointer
 * destination to pair an index with. */
static long distance(char *p, char *base) {
    long d = p++ - base;
    while (*p)
        p++;
    return d + 1;
}

/* A discarded increment stays a bare index bump — the other half of the
 * rule, so that the fix cannot be "wrap everything". */
static int walk(char *p) {
    int n = 0;
    p++;
    while (*p) {
        n += 1;
        p++;
    }
    return n;
}

int main(void) {
    char text[] = "abcd";
    printf("%d %d %d %ld %d\n", count_below(text, text + 4), sum_via_call(text),
           cast_escapes(text), distance(text + 1, text), walk(text));
    return 0;
}
