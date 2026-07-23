#include <stddef.h>
#include <stdio.h>

typedef struct { int *ptr; size_t len; } RustSlice_int;

int find(RustSlice_int arr, int target) {
    int p_index_xj = 0;
    while (p_index_xj < arr.len) {
        if (arr.ptr[p_index_xj] == target)
            return p_index_xj;
        p_index_xj++;
    }
    return -1;
}

int main(void) {
    int d[5] = {10, 20, 30, 40, 50};
    int q = find((RustSlice_int){d, 5}, 30);
    if (q != -1)
        printf("found %d\n", d[q]);
    int r = find((RustSlice_int){d, 5}, 99);
    if (r == -1)
        printf("missing\n");
    return 0;
}
