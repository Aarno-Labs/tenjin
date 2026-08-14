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
        int *q = buf + 4;
        q++;
        s += *q;
    }

    {
        int *q = buf;
        q++;
        int *p = q + 1;
        s += *p + *q;
    }

    return s;
}

int main(void) {
    int b[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    printf("%d\n", f(b));
    return 0;
}
