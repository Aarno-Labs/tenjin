#include <stdio.h>

/* The same ambiguous inheritance source as inherit_shadowed, but with the
 * two `q`s in sibling blocks rather than nested. Picking the wrong `q` is
 * not merely a different program here: the first block's index has gone
 * out of scope by the time the second block's `p` is declared, so the
 * emitted initializer names an identifier that does not exist and the
 * intermediate C no longer parses. */
static int f(int *buf) {
    int s = 0;

    {
        int q_index_xj = 4;
        q_index_xj++;
        s += buf[q_index_xj];
    }

    {
        int q_index_xj_1 = 0;
        q_index_xj_1++;
        int p_index_xj = q_index_xj_1 + (1);
        s += buf[p_index_xj] + buf[q_index_xj_1];
    }

    return s;
}

int main(void) {
    int b[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    printf("%d\n", f(b));
    return 0;
}
