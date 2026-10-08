#include <stdio.h>
#include <string.h>

/* An offset counts elements, and an implicit conversion changes what an
 * element is: `v + 1` is four bytes on, and an index of 1 on a `void *`
 * is one. The right-hand side is kept whole. */

static int second(int *v) {
    void *raw = v + 1;
    int out;
    memcpy(&out, raw, sizeof out);
    return out;
}

/* The same through a plain copy of a pointer that has an index. */
static int third_then_fourth(int *v) {
    int *w = v;
    w += 2;
    const void *raw = w;
    int a, b;
    memcpy(&a, raw, sizeof a);
    raw = w + 1;
    memcpy(&b, raw, sizeof b);
    return a * 100 + b;
}

/* And through the address of an element. */
static int from_element(int k) {
    static int arr[4] = {1, 2, 3, 4};
    const void *raw;
    int out;
    raw = &arr[k];
    memcpy(&out, raw, sizeof out);
    return out;
}

int main(void) {
    int v[] = {10, 20, 30, 40};
    printf("%d %d %d\n", second(v), third_then_fourth(v), from_element(2));
    return 0;
}
