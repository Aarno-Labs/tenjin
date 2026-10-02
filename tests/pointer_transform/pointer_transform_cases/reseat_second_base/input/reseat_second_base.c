#include <stdio.h>

/* The pointer is reseated to a second array. */
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
