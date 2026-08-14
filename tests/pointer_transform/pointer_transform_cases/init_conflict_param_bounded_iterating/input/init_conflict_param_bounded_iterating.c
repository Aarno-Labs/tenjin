#include <stdio.h>

/* A param-bounded pointer whose offset text names another pointer that is
 * itself rewritten.
 *
 * `p`'s offset is source text snapshotted at collect time and pasted back
 * out. It names `q`, and `q` collapses — its declaration is deleted — so the
 * pasted text refers to a variable that no longer exists. The stale-offset
 * step catches exactly this and drops `p`.
 *
 * The catch is that `p` is *param-bounded*: its base is the parameter `buf`
 * and `p < buf + n` resolves against it, which used to mean `p` was emitted
 * without consulting the verdict at all. Re-validation inside
 * transformPointerVar does not save it, because the reason `p` was dropped —
 * a conflict with another pointer's rewrite — is not a fact validation can
 * see. The result was `int p_index_xj = q[0];` with `q` gone: the prepared
 * file does not compile, and a file that does not compile costs the whole
 * codebase its pointer rewrites when the pass rolls back.
 *
 * This differs from init_conflict_param_bounded in one respect, and that is
 * the whole point: there `q` never moves, so it is rejected as non-iterating,
 * never transformed, and the conflict never fires. Here `q++` makes it
 * iterate.
 *
 * The offset is a subscript rather than `q - buf` deliberately. A pointer
 * difference reaches the Unknown catch-all and rejects `q` outright, which
 * also prevents the conflict — masking the bug rather than exercising it.
 *
 * Both shapes that carry an offset are covered: the initializer and a
 * post-declaration assignment. */

/* Initializer form. */
static int init_form(int *buf, int n) {
    int *q = buf + 1;
    int *p = buf + q[0];
    int s = 0;
    while (p < buf + n) {
        s += *p;
        p++;
    }
    q++;
    return s + *q;
}

/* Assignment form: the offset arrives after the declaration. */
static int assign_form(int *buf, int n) {
    int *q = buf + 1;
    int *p;
    int s = 0;
    p = buf + q[0];
    while (p < buf + n) {
        s += *p;
        p++;
    }
    q++;
    return s + *q;
}

int main(void) {
    int a[8] = {0, 1, 2, 4, 8, 16, 32, 64};
    int b[8] = {0, 1, 2, 4, 8, 16, 32, 64};
    printf("%d %d\n", init_form(a, 5), assign_form(b, 5));
    return 0;
}
