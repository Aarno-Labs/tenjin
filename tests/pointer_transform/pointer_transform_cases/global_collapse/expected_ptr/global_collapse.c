#include <stdio.h>

/* A file-scope pointer walking a file-scope array, with a base that is
 * stable and spelled the same way at every access. This is the one global
 * shape the tool collapses, and it must keep collapsing.
 *
 * Its companions global_type_pun and global_reseat pin the cases the
 * global loop has to skip. This one pins that narrowing that loop to
 * Collapse-only did not narrow it to nothing. */
int buf[8];
int gp_index_xj = 0;

static void fill(void) {
    gp_index_xj++;
    buf[gp_index_xj] = 5;
    gp_index_xj++;
    buf[gp_index_xj] = 7;
}

int main(void) {
    fill();
    printf("%d %d %d\n", buf[0], buf[1], buf[2]);
    return 0;
}
