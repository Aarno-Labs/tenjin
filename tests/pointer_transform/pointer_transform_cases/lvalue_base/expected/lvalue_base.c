#include <stdio.h>

/* The case base reconstruction exists for, beside the case that marks its
 * boundary. All three cursors have the same shape — `p` walks the lvalue
 * path `t->storage`, which is not any variable's name — so none of them
 * can be resolved by copy propagation over variables alone. What separates
 * them is what happens in the window between the cursor's declaration and
 * its uses:
 *
 *   checksum            nothing at all                        -> resolves
 *   checksum_and_reset  a store to a *sibling* field           -> resolves
 *   checksum_logged     a call that could name the same object -> does not
 *
 * The middle one is a fact about C's object model rather than an
 * assumption: `t->len` and `t->storage` share the prefix `t` `->` and then
 * diverge at two distinct members, so the store provably does not write
 * the base. The last one is the soundness argument: `t` is a parameter, so
 * the object it points at is reachable from the caller and therefore
 * possibly from `log_start`, which could assign `t->storage` without being
 * handed anything. Substituting there would be a miscompilation.
 *
 * The pair is what makes this file worth having: the two differ by one
 * line, and reconstruction has to tell them apart. */

struct table {
    char *storage;
    unsigned len;
};

static unsigned log_calls = 0;

static void log_start(void) { log_calls++; }

static unsigned checksum(struct table *t) {
    int p_index_xj = 0;
    unsigned sum = 0;
    for (unsigned i = 0; i < t->len; i++)
        sum += (unsigned char)t->storage[p_index_xj++];
    return sum;
}

static unsigned checksum_and_reset(struct table *t) {
    int p_index_xj = 0;
    unsigned sum = 0;
    for (unsigned i = 0; i < t->len; i++)
        sum += (unsigned char)t->storage[p_index_xj++];
    t->len = 0;
    return sum;
}

static unsigned checksum_logged(struct table *t) {
    char *p = t->storage;
    int p_index_xj = 0;
    unsigned sum = 0;
    log_start();
    for (unsigned i = 0; i < t->len; i++)
        sum += (unsigned char)p[p_index_xj++];
    return sum;
}

int main(void) {
    char store[4] = {1, 2, 3, 4};
    struct table t = {store, 4};
    printf("%u ", checksum(&t));
    printf("%u ", checksum_logged(&t));
    printf("%u ", checksum_and_reset(&t));
    printf("%u %u\n", t.len, log_calls);
    return 0;
}
