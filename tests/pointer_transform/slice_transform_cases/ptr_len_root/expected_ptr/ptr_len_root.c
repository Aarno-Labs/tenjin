#include <stdio.h>

static int sum(int *buf, int n) {
    int p_index_xj = 0;
    int s = 0;
    while (p_index_xj < n) {
        s += buf[p_index_xj++];
    }
    return s;
}

int main(void) {
    int data[5] = {1, 2, 3, 4, 5};
    printf("%d\n", sum(data, 5));
    return 0;
}
