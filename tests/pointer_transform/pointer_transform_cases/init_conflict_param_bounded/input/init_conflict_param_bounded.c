#include <stdio.h>

/* `p` is param-bounded — its base is the parameter `buf` and it is compared
 * against `buf + n` — so it is rewritten by the first of the two transform
 * passes. Its later assignment takes an offset that names `q`, another
 * pointer being rewritten, which is exactly the stale-source-text conflict
 * init-conflict detection exists to catch: pasting `(q - buf)` after `q`'s
 * declaration has been deleted leaves a reference to a name that is gone.
 *
 * `p` has no initializer on purpose. With one, the conflict loop looks at
 * the declaration's initializer rather than the assignment's RHS and never
 * sees `q` at all. */
static int f(int *buf, int n) {
    int *q = buf + 1;
    int *p;
    int s = 0;
    p = buf;
    while (p < buf + n) {
        s += *p;
        p++;
    }
    p = buf + (q - buf);
    return s + *p + *q;
}

int main(void) {
    int a[8] = {1, 2, 4, 8, 16, 32, 64, 128};
    printf("%d\n", f(a, 5));
    return 0;
}
