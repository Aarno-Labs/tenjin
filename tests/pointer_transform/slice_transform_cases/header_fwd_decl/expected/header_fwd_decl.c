#include <stdio.h>

#include "lib.h"

int sum(RustSlice_int arr) {
    int p_index_xj = 0;
    int s = 0;
    while (p_index_xj < arr.len) {
        s += arr.ptr[p_index_xj++];
    }
    return s;
}

int main(void) {
    int d[5] = {2, 4, 6, 8, 10};
    printf("%d\n", sum((RustSlice_int){d, 5}));
    return 0;
}
