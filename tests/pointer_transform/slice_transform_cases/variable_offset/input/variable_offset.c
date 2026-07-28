#include <stdio.h>

static int sum_stride(int *buf, int n, int j) {
    int *p = buf;
    int s = 0;
    while (p < buf + n) {
        s += p[j];
        p++;
    }
    return s;
}

static int sum_plain(int *buf, int n) {
    int *p = buf;
    int s = 0;
    while (p < buf + n) {
        s += *p++;
    }
    return s;
}

int main(void) {
    int data[6] = {1, 2, 3, 4, 5, 6};
    printf("%d %d\n", sum_stride(data, 3, 2), sum_plain(data, 5));
    return 0;
}
