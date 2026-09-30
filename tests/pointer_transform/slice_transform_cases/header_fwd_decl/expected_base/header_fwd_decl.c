#include <stdio.h>

#include "lib.h"

int sum(int *buf, int n) {
    int p_index_xj = 0;
    int s = 0;
    while (p_index_xj < n) {
        s += buf[p_index_xj++];
    }
    return s;
}

int main(void) {
    int d[5] = {2, 4, 6, 8, 10};
    printf("%d\n", sum(d, 5));
    return 0;
}
