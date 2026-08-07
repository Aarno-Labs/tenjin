#include <stdio.h>

/* A file-scope pointer reseated to a second array. markReseat sets
 * collapse_ineligible and emits AssignPtr, and generateGlobalTransformation
 * has no case for AssignPtr — so collapsing this leaves `gp = b` verbatim
 * after `gp`'s declaration has been replaced (invalid C: 'gp' undeclared)
 * while every access collapses onto `a`, the array the pointer was reseated
 * away from.
 *
 * Both arrays are printed so a collapse onto the wrong one is observable
 * as a value difference and not only as a syntax error. Expect no edits. */
int a[8], b[8];
int *gp = a;

int main(void) {
    gp = b;
    gp++;
    gp[0] = 3;
    printf("%d %d\n", a[1], b[1]);
    return 0;
}
