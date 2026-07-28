#include <stdio.h>

static int sum(int *buf, int n) {
    int *p = buf;
    int s = 0;
    while (p < buf + n) {
        s += *p++;
    }
    return s;
}

int main(void) {
    int data[5] = {1, 2, 3, 4, 5};
    printf("%d\n", sum(data, 5));
    return 0;
}
