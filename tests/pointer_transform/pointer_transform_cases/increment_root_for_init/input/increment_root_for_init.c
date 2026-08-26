#include <stdio.h>

/* A stepped root declared in the same for-init as the pointer that steps it.
 * Two declarators means collapse mode, so both indices are hoisted to before
 * the loop, and `q`'s index reads — and advances — `p`'s:
 *
 *     int p_index_xj = 0;
 *     int q_index_xj = p_index_xj++;
 *     for (int *p = a, *q = p; ...)
 *
 * That is correct only because the hoists land in declaration order, so
 * `p_index_xj` exists and holds its initial value by the time `q`'s index
 * reads it. The scope guard does not decide this: it inspects the *offset*
 * expressions and never the root, so a root that is a for-init sibling is
 * allowed through. What keeps it sound is the ordering of the hoists, which
 * is why the case is worth pinning separately from the offset-scope ones.
 *
 * `for_init_multi_dependent` is the contrast: there the offset names a
 * non-pointer bound by the same statement, which cannot be hoisted at all. */

static int paired(int *a, int n) {
    int s = 0;
    for (int *p = a, *q = p++; p - a < n; p++, q++)
        s += *p * 2 + *q;
    return s;
}

/* The same shape with the step on the second declarator instead, so the
 * hoisted index that carries the step is the later of the two. */
static int paired_second(int *a, int n) {
    int s = 0;
    for (int *p = a, *q = p + 1; p - a < n; p++, q++)
        s += *p + *q * 3;
    return s;
}

int main(void) {
    int v[8] = {1, 2, 4, 8, 16, 32, 64, 128};
    printf("%d %d\n", paired(v, 4), paired_second(v, 4));
    return 0;
}
