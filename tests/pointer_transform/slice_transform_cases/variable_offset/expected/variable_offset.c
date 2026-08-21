#include <stdio.h>

static int sum_stride(int *buf, int n, int j) {
    int p_index_xj = 0;
    int s = 0;
    while (p_index_xj < n) {
        s += buf[p_index_xj + j];
        p_index_xj++;
    }
    return s;
}

typedef struct { int *ptr; size_t len; } RustSlice_int;

static int sum_plain(RustSlice_int arr) {
    int p_index_xj = 0;
    int s = 0;
    while (p_index_xj < arr.len) {
        s += arr.ptr[p_index_xj++];
    }
    return s;
}

int main(void) {
    int data[6] = {1, 2, 3, 4, 5, 6};
    printf("%d %d\n", sum_stride(data, 3, 2), sum_plain((RustSlice_int){data, 5}));
    return 0;
}
