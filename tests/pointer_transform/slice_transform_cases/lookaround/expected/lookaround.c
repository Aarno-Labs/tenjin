#include <stdio.h>

/* Exercises lookback (via *(p - 1)) and lookahead (via *(p + 1)): the
 * constructed slice at the call site must be widened on both sides. */
typedef struct { int *ptr; size_t len; } RustSlice_int;

static int lookaround(RustSlice_int arr) {
    int p_index_xj = 1 + 1;
    int i = 1;
    int total = 0;
    while (p_index_xj < arr.len - 1) {
        total += arr.ptr[p_index_xj - 1];
        if (i + 1 < (arr.len - 2))
            total += arr.ptr[p_index_xj + 1];
        i++;
        p_index_xj++;
    }
    return total;
}

int main(void) {
    int d[5] = {1, 2, 3, 4, 5};
    printf("%d\n", lookaround((RustSlice_int){d - 1, 5 + 1 + 1}));
    return 0;
}
