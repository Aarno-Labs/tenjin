/* Same-named `static` functions in different files must not share
 * detection state. uniquify_statics runs after these passes, so a bare
 * function name (or a file basename) is not a program-wide identity:
 *
 *   sum      — slice-detectable in a.c, deliberately not in b.c
 *   pick     — a global-return function in a.c, not one in b.c
 *   scale    — same basename (src1/util.c, src2/util.c), different
 *              lookback, so a merged record would widen the wrong one
 *
 * Applying either file's record to the other's function produces C that
 * does not compile or that reads the wrong elements, so this case is
 * guarded by the driver's syntax check and behavior comparison.
 */
#include <stdio.h>

int a_sum(const int *buf, int n);
int b_sum(const int *buf, int n);
int a_pick(int i);
int b_pick(int i);
int src1_scale(const int *buf, int n);
int src2_scale(const int *buf, int n);

int main(void) {
    int d[5] = {1, 2, 3, 4, 5};
    printf("%d %d\n", a_sum(d, 5), b_sum(d, 5));
    printf("%d %d\n", a_pick(1), b_pick(1));
    printf("%d %d\n", src1_scale(d, 5), src2_scale(d, 5));
    return 0;
}
