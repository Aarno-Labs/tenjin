#include <stdio.h>

/* The pointer's pointee type differs from the array it was taken from.
 * Indexing the base by text would have gone through the wrong element
 * type; a retained pointer keeps its own, so the byte walk stays a byte
 * walk. Every value is under 256, so the sum is byte-order independent. */
static int checksum(int *ints, int n) {
    unsigned char *p = (unsigned char *)ints;
    int p_index_xj = 0;
    int total = 0;
    for (int i = 0; i < n * (int)sizeof(int); i++)
        total += p[p_index_xj++];
    return total;
}

int main(void) {
    int v[4] = {1, 2, 3, 4};
    printf("%d\n", checksum(v, 4));
    return 0;
}
