#include <stdio.h>

/* Same stale-offset hazard as init_offset_names_transformed_ptr, but on a
 * post-declaration assignment rather than the initializer.
 *
 * The initializer on `p` is load-bearing. Init-conflict detection used to
 * pick the expression to inspect by asking whether the *pointer* had an
 * initializer rather than what kind the *access* was, so for an assignment on
 * an initialized pointer it examined `buf` — the declaration's initializer —
 * and never looked at the assignment's own right-hand side. Drop the `= buf`
 * and the conflict is found, which is what init_conflict_param_bounded
 * covers; keep it and the conflict is missed. */
static int f(int *buf, int n) {
    int *q = buf + 1;
    int *p = buf;
    int s = 0;
    while (p < buf + n) {
        s += *p;
        p++;
    }
    p = buf + (q - buf);
    return s + *p + *q;
}

int main(void) {
    int a[8] = {1, 2, 4, 8, 16, 32, 64, 128};
    printf("%d\n", f(a, 5));
    return 0;
}
