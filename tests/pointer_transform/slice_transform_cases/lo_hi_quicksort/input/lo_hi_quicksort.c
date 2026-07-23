#include <stdio.h>

static int *partition(int *lo, int *hi) {
    int pivot = *hi;
    int *i = lo;
    int *j = lo;
    while (j < hi) {
        if (*j < pivot) {
            int t = *i;
            *i = *j;
            *j = t;
            i++;
        }
        j++;
    }
    int t = *i;
    *i = *hi;
    *hi = t;
    return i;
}

static void quick_sort(int *lo, int *hi) {
    if (lo < hi) {
        int *p = partition(lo, hi);
        quick_sort(lo, p - 1);
        quick_sort(p + 1, hi);
    }
}

int main(void) {
    int d[7] = {3, 7, 1, 6, 2, 5, 4};
    quick_sort(d, d + 6);
    for (int i = 0; i < 7; i++)
        printf("%d ", d[i]);
    printf("\n");
    return 0;
}
