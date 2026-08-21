#include <stdio.h>

typedef struct { int *ptr; size_t len; } RustSlice_int;

static int sum_range(RustSlice_int arr) {
    int total = 0;
    int p_index_xj = 0;
    while (p_index_xj < arr.len) {
        total += arr.ptr[p_index_xj];
        p_index_xj++;
    }
    return total;
}

static int sum_all(RustSlice_int arr) {
    return sum_range(arr);
}

int main(void) {
    int d[5] = {1, 2, 3, 4, 5};
    printf("%d\n", sum_all((RustSlice_int){d, 5}));
    return 0;
}
