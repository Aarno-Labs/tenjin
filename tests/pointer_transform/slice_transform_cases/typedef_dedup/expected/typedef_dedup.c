#include <stdio.h>

/* Two int-slice functions must share one RustSlice_int typedef; the char
 * function gets its own RustSlice_char typedef. */
typedef struct { int *ptr; size_t len; } RustSlice_int;

static int sum_ints(RustSlice_int arr) {
    int p_index_xj = 0;
    int s = 0;
    while (p_index_xj < arr.len) {
        s += arr.ptr[p_index_xj++];
    }
    return s;
}

static int max_int(RustSlice_int arr) {
    int p_index_xj = 0;
    int best = -1000000;
    while (p_index_xj < arr.len) {
        if (arr.ptr[p_index_xj] > best)
            best = arr.ptr[p_index_xj];
        p_index_xj++;
    }
    return best;
}

typedef struct { char *ptr; size_t len; } RustSlice_char;

static int count_upper(RustSlice_char arr) {
    int p_index_xj = 0;
    int count = 0;
    while (p_index_xj < arr.len) {
        if (arr.ptr[p_index_xj] >= 'A' && arr.ptr[p_index_xj] <= 'Z')
            count++;
        p_index_xj++;
    }
    return count;
}

int main(void) {
    int d[4] = {4, 8, 15, 16};
    char s[5] = {'a', 'B', 'c', 'D', 'e'};
    printf("%d %d %d\n", sum_ints((RustSlice_int){d, 4}), max_int((RustSlice_int){d, 4}), count_upper((RustSlice_char){s, 5}));
    return 0;
}
