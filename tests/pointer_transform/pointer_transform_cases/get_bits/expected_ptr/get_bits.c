#include <stdint.h>
#include <stdio.h>

static uint32_t get_bits(const uint8_t *p, int n) {
    int p_index_xj = 0;
    uint32_t next, cache = 0, s = n & 7;
    int shl = n + s;
    next = p[p_index_xj++] & (255 >> s);
    while ((shl -= 8) > 0) {
        cache |= next << shl;
        next = p[p_index_xj++];
    }
    return cache | (next >> -shl);
}

int main(void) {
    uint8_t data[4] = {0xde, 0xad, 0xbe, 0xef};
    printf("%u\n", get_bits(data, 16));
    printf("%u\n", get_bits(data, 12));
    return 0;
}
