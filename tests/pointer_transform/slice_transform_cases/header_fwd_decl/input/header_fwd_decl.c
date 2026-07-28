#include <stdio.h>

#include "lib.h"

int sum(int *buf, int n) {
    int *p = buf;
    int s = 0;
    while (p < buf + n) {
        s += *p++;
    }
    return s;
}

int main(void) {
    int d[5] = {2, 4, 6, 8, 10};
    printf("%d\n", sum(d, 5));
    return 0;
}
