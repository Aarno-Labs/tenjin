#include <stdio.h>

/* q is declared as a plain copy of p. */
typedef struct { int *ptr; size_t len; } RustSlice_int;

static int tail_sum(RustSlice_int arr) {
    int p_index_xj = 0;
    int s = 0;
    for (int i = 0; i < arr.len / 2; i++) {
        s += arr.ptr[p_index_xj];
        p_index_xj++;
    }
    int q_index_xj = p_index_xj;
    while (q_index_xj < arr.len) {
        s += arr.ptr[q_index_xj] * 2;
        q_index_xj++;
    }
    return s;
}

int main(void) {
    int v[6] = {1, 2, 3, 4, 5, 6};
    printf("%d\n", tail_sum((RustSlice_int){v, 6}));
    return 0;
}
