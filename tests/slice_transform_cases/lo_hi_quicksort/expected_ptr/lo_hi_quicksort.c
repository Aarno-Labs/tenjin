#include <stdio.h>

static int *partition(int *lo, int *hi) {
    int pivot = *hi;
    int i_index_xj = 0;
    int j_index_xj = 0;
    while (j_index_xj < (hi - lo)) {
        if (lo[j_index_xj] < pivot) {
            int t = lo[i_index_xj];
            lo[i_index_xj] = lo[j_index_xj];
            lo[j_index_xj] = t;
            i_index_xj++;
        }
        j_index_xj++;
    }
    int t = lo[i_index_xj];
    lo[i_index_xj] = *hi;
    *hi = t;
    return lo + i_index_xj;
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
