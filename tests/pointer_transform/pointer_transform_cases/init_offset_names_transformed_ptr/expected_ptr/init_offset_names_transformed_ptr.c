#include <stdio.h>

/* `p`'s initializer offset names `q`, another pointer being rewritten. The
 * offset is source text snapshotted at collect time and pasted back out, so
 * once `q`'s declaration is deleted that text names a variable that is gone.
 *
 * `q` sits in the *offset*, not the base, so index inheritance does not apply
 * and init-conflict detection has to catch it. It used not to: the conflict
 * loop walked the whole initializer, which made `T *p = q + 1` look like a
 * conflict too, and the exemption added to suppress that false positive —
 * "the reference is materialized, so the text is fine" — also suppressed this
 * real one. The materialization never survives: the declaration edit replaces
 * the whole statement with text built from the stale snapshot, and applyEdits
 * drops the inner rewrite as overlapping. */
static int f(int *buf, int n) {
    int q_index_xj = 1;
    int *p = buf + ((buf + q_index_xj) - buf);
    int s = 0;
    while (p < buf + n) {
        s += *p;
        p++;
    }
    return s + buf[q_index_xj];
}

int main(void) {
    int a[8] = {1, 2, 4, 8, 16, 32, 64, 128};
    printf("%d\n", f(a, 5));
    return 0;
}
