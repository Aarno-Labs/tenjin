#include <stddef.h>
#include <stdio.h>

static int *find(int *buf, int n, int target) {
    int *p = buf;
    int p_index_xj = 0;
    while ((p + p_index_xj) < buf + n) {
        if (p[p_index_xj] == target)
            return (p + p_index_xj);
        p_index_xj++;
    }
    return NULL;
}

int main(void) {
    int d[5] = {10, 20, 30, 40, 50};
    int *q = find(d, 5, 30);
    if (q)
        printf("found %d\n", *q);
    int *r = find(d, 5, 99);
    if (!r)
        printf("missing\n");
    return 0;
}
