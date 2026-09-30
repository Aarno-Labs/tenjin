#include <stdio.h>

/* Unbraced substatements nested several deep. Only the innermost
 * construct — the one holding the for-init — needs a block: wrapping it
 * turns it into a single statement, which is exactly what each enclosing
 * construct already expected, however many of them there are.
 *
 * The dangling-else and label cases are the ones worth pinning: the
 * block has to swallow the loop's semicolon, or the `else` binds to the
 * wrong `if` — or fails to parse at all. */

/* if / while / for — three levels, no braces anywhere. */
static int if_while_for(int *a, int n, int c) {
    int s = 0;
    int rounds = 2;
    if (c)
        while (rounds-- > 0)
            { int q_index_xj = 1;
            { int p_index_xj = 0;
            for (int *p = a, *q = a + 1; p_index_xj < n; p_index_xj++, q_index_xj++)
                s += a[p_index_xj] * 2 + a[q_index_xj];
            }
            }
    return s;
}

/* Dangling else: the `else` belongs to the inner `if`, and must still
 * belong to it after the loop is wrapped. */
static int dangling_else(int *a, int n, int c, int d) {
    int s = 0;
    if (c)
        if (d)
            { int q_index_xj = 1;
            { int p_index_xj = 0;
            for (int *p = a, *q = a + 1; p_index_xj < n; p_index_xj++, q_index_xj++)
                s += a[p_index_xj] + a[q_index_xj];
            }
            }
        else
            s = -1;
    return s;
}

/* Four levels: if / for / while / for. */
static int four_levels(int *a, int n, int c) {
    int s = 0;
    if (c)
        for (int i = 0; i < 2; i++) {
            int rounds = 1;
            while (rounds-- > 0)
                { int q_index_xj = 1;
                { int p_index_xj = 0;
                for (int *p = a, *q = a + 1; p_index_xj < n; p_index_xj++, q_index_xj++)
                    s += (a[p_index_xj] - a[q_index_xj]) * (i + 1);
                }
                }
        }
    return s;
}

/* A frozen handle at depth, reached through a label inside an unbraced
 * `if`. The label sits outside the block the index gets. */
static int labelled_at_depth(int *a, int *b, int n, int c) {
    int s = 0;
    if (c)
    retry:
        for (int *p = a; n > 0; n--) {
            if (n == 2)
                p = b;
            s += *p;
            p++;
        }
    if (s < 0)
        goto retry;
    return s;
}

/* `do` inside an unbraced `while`, loop body an expression statement:
 * the block closes after the semicolon but before the inner `while`. */
static int do_inside_while(int *a, int n) {
    int s = 0;
    int outer = 2;
    while (outer-- > 0)
        do
            { int q_index_xj = 1;
            { int p_index_xj = 0;
            for (int *p = a, *q = a + 1; p_index_xj < n; p_index_xj++, q_index_xj++)
                s += a[p_index_xj] | a[q_index_xj];
            }
            }
        while (0);
    return s;
}

int main(void) {
    int a[8] = {1, 2, 4, 8, 16, 32, 64, 128};
    int b[8] = {3, 5, 7, 9, 11, 13, 15, 17};
    printf("%d %d %d %d %d %d\n", if_while_for(a, 3, 1),
           dangling_else(a, 3, 1, 1), dangling_else(a, 3, 1, 0),
           four_levels(a, 3, 1), labelled_at_depth(a, b, 3, 1),
           do_inside_while(a, 3));
    return 0;
}
