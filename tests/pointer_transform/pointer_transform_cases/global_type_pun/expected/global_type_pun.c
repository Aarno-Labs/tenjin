#include <stdio.h>

/* A file-scope pointer whose pointee type differs from its base array's
 * element type. Collapsing it would rewrite `*fp = 1.0f` into
 * `ibuf[fp_index] = 1.0f`, which float-to-int converts instead of storing
 * the IEEE-754 bit pattern — 1 where the original wrote 1065353216.
 *
 * The type-punning check demotes this to handle mode. Globals have no
 * handle path (generateGlobalTransformation always collapses), so the
 * global loop must skip it rather than collapse onto the base the
 * validator just judged unsafe to substitute. Expect no edits at all. */
int ibuf[8];
float *fp = (float *)ibuf;

int main(void) {
    fp++;
    *fp = 1.0f;
    printf("%d\n", ibuf[1]);
    return 0;
}
