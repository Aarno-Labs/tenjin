#include <stdio.h>

typedef struct { int *ptr; size_t len; } RustSlice_int;

static void swap(RustSlice_int arr, int a, int b) {
    int t = arr.ptr[a];
    arr.ptr[a] = arr.ptr[b];
    arr.ptr[b] = t;
}

static void sort(RustSlice_int arr) {
    int done = 0;
    while (!done) {
        done = 1;
        int i = 0;
        int p_index_xj = 0;
        while (p_index_xj < arr.len) {
            if (i > 0) {
                if (arr.ptr[p_index_xj + -1] > arr.ptr[p_index_xj]) {
                    swap(arr, p_index_xj - 1, p_index_xj);
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
    sort((RustSlice_int){d, 6});
    for (int i = 0; i < 6; i++)
        printf("%d ", d[i]);
    printf("\n");
    return 0;
}
