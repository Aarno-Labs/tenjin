#include <stdio.h>

/* The root of a pairwise assignment emits no edit of its own, because the
 * owner's rewrite carries the pair. When the owner turns out *not* to be
 * rewritten, the root has to do for itself whatever the owner would have done
 * for it — and for a stepped root that is not a value read, it is the
 * increment again.
 *
 * Here `q` is a candidate on the strength of the offset its initializer lands
 * at, then fails validation because its address is taken. So `p`'s reference
 * inside `q = p++` is demoted, and demoting it to a plain value read would
 * drop the `++` outright: `p` would never advance and the walk below would not
 * terminate the same way. It has to come back as an increment, rendered in
 * value position because the initializer still wants a pointer.
 *
 * Validation has to anticipate this. A pairwise root is held to the standard
 * of what it may be demoted *into*, and for a stepped root that is the whole
 * `p++`, a wider span than the bare reference — there is no second chance to
 * refuse once the demotion has happened. */

static void bump(char **pp) { (*pp)++; }

static int scan(char *p) {
    int p_index_xj = 0;
    char *q = (p + p_index_xj++);
    bump(&q);

    int n = 0;
    while (p[p_index_xj]) {
        n += 1;
        p_index_xj++;
    }
    return n + (int)((p + p_index_xj) - q);
}

int main(void) {
    char text[] = "abcde";
    printf("%d\n", scan(text));
    return 0;
}
