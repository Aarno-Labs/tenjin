#include <stdio.h>

/* Each pointer is initialized from the previous one plus an offset. */
typedef struct { int *ptr; size_t len; } RustSlice_int;

static int chain(RustSlice_int arr) {
    int p_index_xj = 0;
    int q_index_xj = p_index_xj + 1;
    int r_index_xj = q_index_xj + 1;
    int s = 0;
    while (r_index_xj < arr.len) {
        s += arr.ptr[p_index_xj] + arr.ptr[q_index_xj] * 2 + arr.ptr[r_index_xj] * 4;
        p_index_xj++;
        q_index_xj++;
        r_index_xj++;
    }
    return s;
}

int main(void) {
    int v[6] = {1, 2, 3, 4, 5, 6};
    printf("%d\n", chain((RustSlice_int){v, 6}));
    return 0;
}
