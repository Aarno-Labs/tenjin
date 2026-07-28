/* The undetectable half of each colliding pair: same names as a.c's
 * statics, different shapes. Neither may inherit a.c's records. */

#include <stddef.h>

static int spare[4] = {1, 1, 1, 1};

/* Not slice-detectable: no moving pointer, and buf escapes into a
 * pointer-valued global rather than being iterated. */
static const int *last_seen;

static int sum(const int *buf, int n) {
    last_seen = buf;
    return buf[0] + n;
}

/* Not a global-return function: returns a pointer that is not an
 * element of a global array, so its return type must stay `int *`. */
static int *pick(int i) {
    static int local_slot;
    local_slot = spare[i & 3] + i;
    return &local_slot;
}

int b_sum(const int *buf, int n) { return sum(buf, n); }

int b_pick(int i) {
    int *p = pick(i);
    return p ? *p : -1;
}
