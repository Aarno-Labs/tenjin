#include <stdio.h>

/* Two same-named pointers whose indices are hoisted into the *same*
 * block. Each `for` is a multi-declarator init, so each index leaves the
 * loop header and lands in the enclosing compound statement — and both
 * loops share that compound statement.
 *
 * Deriving the index name from the pointer's name would emit
 * `int p_index_xj = 0;` twice into one block: a redefinition, and the
 * prepared file would not compile. Names are assigned per pointer
 * instead, so the second pair becomes p_index_xj_1 / q_index_xj_1.
 *
 * Without hoisting this shape is harmless — each index would replace its
 * own declaration inside its own loop header — which is why the naming
 * and the placement have to land together. */
static int siblings(int *a, int n) {
    int s = 0;
    int q_index_xj = 1;
    int p_index_xj = 0;
    for (int *p = a, *q = a + 1; p_index_xj < n; p_index_xj++, q_index_xj++)
        s += a[p_index_xj] + a[q_index_xj];
    int q_index_xj_1 = 2;
    int p_index_xj_1 = 0;
    for (int *p = a, *q = a + 2; p_index_xj_1 < n; p_index_xj_1++, q_index_xj_1++)
        s += a[p_index_xj_1] * 2 + a[q_index_xj_1];
    return s;
}

/* The same collision one level down: an inner block reusing the name of
 * a pointer hoisted in the outer one. These are genuinely different
 * scopes, so distinct names are not strictly required here — but the
 * assignment is per function, and taking the conservative branch keeps
 * the rule "one pointer, one name" rather than making it depend on a
 * scope analysis. */
static int nested_scopes(int *a, int n) {
    int s = 0;
    int q_index_xj = 1;
    int p_index_xj = 0;
    for (int *p = a, *q = a + 1; p_index_xj < n; p_index_xj++, q_index_xj++)
        s += a[p_index_xj] + a[q_index_xj];
    {
        int q_index_xj_1 = 3;
        int p_index_xj_1 = 0;
        for (int *p = a, *q = a + 3; p_index_xj_1 < n; p_index_xj_1++, q_index_xj_1++)
            s += a[p_index_xj_1] - a[q_index_xj_1];
    }
    return s;
}

int main(void) {
    int d[6] = {1, 2, 4, 8, 16, 32};
    printf("%d %d\n", siblings(d, 3), nested_scopes(d, 3));
    return 0;
}
