#include <stdio.h>

static void swap(int *a, int *b) {
    int t = *a;
    *a = *b;
    *b = t;
}

static void sort(int *buf, int n) {
    int done = 0;
    while (!done) {
        done = 1;
        int i = 0;
        int p_index_xj = 0;
        while (p_index_xj < n) {
            if (i > 0) {
                if (buf[p_index_xj + -1] > buf[p_index_xj]) {
                    swap(buf + p_index_xj - 1, buf + p_index_xj);
                    done = 0;
                }
            }
            i++;
            p_index_xj++;
        }
    }
}

int main(void) {
    int d[6] = {3, 1, 4, 1, 5, 9};
    sort(d, 6);
    for (int i = 0; i < 6; i++)
        printf("%d ", d[i]);
    printf("\n");
    return 0;
}
