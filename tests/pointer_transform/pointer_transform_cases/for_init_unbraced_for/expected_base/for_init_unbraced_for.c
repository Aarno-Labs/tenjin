#include <stdio.h>

/* An index declaration needs a statement slot that admits a declaration.
 * As the unbraced substatement of an if / else / while / for / do, or
 * after a label or a `case`, C admits a statement but not a declaration
 * — and a bare insertion there would also detach the loop from the
 * construct that owns it. Each such anchor is wrapped in a fresh block.
 *
 * Both shapes that place an index before the loop are covered: a
 * collapse-mode multi-declarator for-init, and a frozen handle. A
 * single-declarator collapse needs no block, since it replaces its own
 * declaration in place.
 *
 * Where the loop body is an expression statement the loop ends *before*
 * its semicolon, so the closing brace has to step over it. */

/* Unbraced `if`. Two pointers wrap the same loop, so the blocks nest. */
static int in_if(int *a, int n, int c) {
    int s = 0;
    if (c)
        { int q_index_xj = 1;
        { int p_index_xj = 0;
        for (int *p = a, *q = a + 1; p_index_xj < n; p_index_xj++, q_index_xj++)
            s += a[p_index_xj] * 2 + a[q_index_xj];
        }
        }
    return s;
}

/* Unbraced `else`, holding a frozen handle: the block has to close
 * inside the else-branch or the `if` no longer parses. */
static int in_else(int *a, int *b, int n, int c) {
    int s = 0;
    if (c)
        s = -1;
    else
        for (int *p = a; n > 0; n--) {
            if (n == 2)
                p = b;
            s += *p;
            p++;
        }
    return s;
}

/* Unbraced `while` body. */
static int in_while(int *a, int n) {
    int s = 0;
    int rounds = 2;
    while (rounds-- > 0)
        { int q_index_xj = 1;
        { int p_index_xj = 0;
        for (int *p = a, *q = a + 1; p_index_xj < n; p_index_xj++, q_index_xj++)
            s += a[p_index_xj] - a[q_index_xj];
        }
        }
    return s;
}

/* Unbraced body of an outer `for`. The index is re-initialized on every
 * outer pass, exactly as the inner init clause is. */
static int in_for(int *a, int n) {
    int s = 0;
    for (int i = 0; i < 2; i++)
        { int q_index_xj = 1;
        { int p_index_xj = 0;
        for (int *p = a, *q = a + 1; p_index_xj < n; p_index_xj++, q_index_xj++)
            s += (a[p_index_xj] + a[q_index_xj]) * (i + 1);
        }
        }
    return s;
}

/* `do` body: the block has to close before the `while`. */
static int in_do(int *a, int n) {
    int s = 0;
    int rounds = 2;
    do
        { int q_index_xj = 1;
        { int p_index_xj = 0;
        for (int *p = a, *q = a + 1; p_index_xj < n; p_index_xj++, q_index_xj++)
            s += a[p_index_xj] + a[q_index_xj];
        }
        }
    while (--rounds > 0);
    return s;
}

/* After a `case` label, with the `break` a sibling of the loop. */
static int in_case(int *a, int n, int k) {
    int s = 0;
    switch (k) {
    case 1:
        { int q_index_xj = 1;
        { int p_index_xj = 0;
        for (int *p = a, *q = a + 1; p_index_xj < n; p_index_xj++, q_index_xj++)
            s += a[p_index_xj] | a[q_index_xj];
        }
        }
        break;
    default:
        s = -1;
        break;
    }
    return s;
}

/* After a plain label, jumped back to from below. */
static int in_label(int *a, int n) {
    int s = 0;
    int again = 1;
retry:
    { int q_index_xj = 1;
    { int p_index_xj = 0;
    for (int *p = a, *q = a + 1; p_index_xj < n; p_index_xj++, q_index_xj++)
        s += a[p_index_xj] ^ a[q_index_xj];
    }
    }
    if (again) {
        again = 0;
        goto retry;
    }
    return s;
}

int main(void) {
    int a[8] = {1, 2, 4, 8, 16, 32, 64, 128};
    int b[8] = {3, 5, 7, 9, 11, 13, 15, 17};
    printf("%d %d %d %d %d %d %d %d\n", in_if(a, 3, 1), in_if(a, 3, 0),
           in_else(a, b, 3, 0), in_while(a, 3), in_for(a, 3), in_do(a, 3),
           in_case(a, 3, 1), in_label(a, 3));
    return 0;
}
