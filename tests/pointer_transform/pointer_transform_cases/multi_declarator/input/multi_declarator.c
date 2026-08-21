#include <stdio.h>

/* Two cursors declared by one statement, both walking the same base.
 * Either one alone would reconstruct; together they cannot, because
 * deleting a pointer means deleting its declaration and this declaration
 * declares something else too.
 *
 * The property under test is that the pair is *declined*, not that it is
 * miscompiled: what comes out is exactly what the pointer pass emitted,
 * which already runs. Single-declarator is the precondition, and this is
 * the shape that would be silently wrong without it. */

static int pairwise(int *a, int n) {
    int *p = a, *q = a + 1;
    int s = 0;
    for (int i = 0; i + 1 < n; i++)
        s += *p++ * 2 + *q++;
    return s;
}

int main(void) {
    int v[5] = {1, 2, 3, 4, 5};
    printf("%d\n", pairwise(v, 5));
    return 0;
}
