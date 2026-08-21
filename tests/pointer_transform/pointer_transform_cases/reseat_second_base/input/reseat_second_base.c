#include <stdio.h>

/* One pointer, two bases. There is no single base to substitute, which
 * is what used to disqualify the pointer; the retained form just takes
 * the new base and restarts the index beside it. */
static int walk_two(int *a, int na, int *b, int nb) {
    int *p = a;
    int total = 0;
    for (int i = 0; i < na; i++)
        total += *p++;
    p = b;
    for (int i = 0; i < nb; i++)
        total += *p++;
    return total;
}

int main(void) {
    int x[3] = {1, 2, 3};
    int y[2] = {10, 20};
    printf("%d\n", walk_two(x, 3, y, 2));
    return 0;
}
