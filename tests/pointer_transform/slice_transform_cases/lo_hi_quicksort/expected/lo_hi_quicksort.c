#include <stdio.h>

typedef struct { int *ptr; size_t len; } RustSlice_int;

int partition(RustSlice_int arr) {
    int pivot = arr.ptr[(arr.len - 1)];
    int i_index_xj = 0;
    int j_index_xj = 0;
    while (j_index_xj < arr.len - 1) {
        if (arr.ptr[j_index_xj] < pivot) {
            int t = arr.ptr[i_index_xj];
            arr.ptr[i_index_xj] = arr.ptr[j_index_xj];
            arr.ptr[j_index_xj] = t;
            i_index_xj++;
        }
        j_index_xj++;
    }
    int t = arr.ptr[i_index_xj];
    arr.ptr[i_index_xj] = arr.ptr[(arr.len - 1)];
    arr.ptr[(arr.len - 1)] = t;
    return i_index_xj;
}

static void quick_sort(RustSlice_int arr) {
    if (arr.len > 1) {
        int p = partition(arr);
        quick_sort((RustSlice_int){arr.ptr, p});
        quick_sort((RustSlice_int){arr.ptr + p + 1, arr.len - (p + 1)});
    }
}

int main(void) {
    int d[7] = {3, 7, 1, 6, 2, 5, 4};
    quick_sort((RustSlice_int){d, 6 + 1});
    for (int i = 0; i < 7; i++)
        printf("%d ", d[i]);
    printf("\n");
    return 0;
}
