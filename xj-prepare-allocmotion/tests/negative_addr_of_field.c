/* NEGATIVE: the address of a field is taken (`&p->y`) and escapes into a
 * call. After boxing, the field lives on the heap, so a stack address would
 * dangle -- the pass must leave this untouched. */
#include <stdlib.h>

struct Point {
    int x;
    int y;
};

extern void use(struct Point *p);
extern void stash(int *slot);

void neg_addr_of_field(int a, int b) {
    struct Point *p = malloc(sizeof(struct Point));
    p->x = a;
    p->y = b;
    stash(&p->y);
    use(p);
}
