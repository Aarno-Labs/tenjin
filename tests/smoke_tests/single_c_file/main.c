#include <stdio.h>
#include <assert.h>

int get_bits(const short *p, int n) {
    int next, cache = 0, s = n & 7;
    int shl = n + s;
    next = *p++ & (255 >> s);  // <- pointer modified
    while ((shl-= 8) > 0) {
        cache |= next << shl;
        next = *p++;           // <- pointer modified
    }
    return cache | (next >> -shl);
}

int main(int argc, char** argv)
{
	printf("Hello, Tenjin!\n");
	assert(1);
	
	if (argc > 2) {
          printf("  (more than two args provided)\n");
	}
	return 0;
}
