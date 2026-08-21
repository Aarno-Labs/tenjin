#include <stdio.h>

typedef struct { int *ptr; size_t len; } RustSlice_int;

static int sum(RustSlice_int arr) {
    int p_index_xj = 0;
    int s = 0;
    while (p_index_xj < arr.len) {
        s += arr.ptr[p_index_xj++];
    }
    return s;
}

int main(void) {
    int data[5] = {1, 2, 3, 4, 5};
    printf("%d\n", sum((RustSlice_int){data, 5}));
    return 0;
}
