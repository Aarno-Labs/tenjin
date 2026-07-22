// XREF:ffi_export_wrapper
// The second source file in the multi-file codebase. Its wrapped functions land
// in the same shared `xj_ffi` module as those from `strings.c`.

#include "api.h"

int sum_n(int *xs, int n)
{
    int total = 0;
    for (int i = 0; i < n; i++)
    {
        total += xs[i];
    }
    return total;
}

int bump(int *p)
{
    *p += 1;
    return *p;
}

// Internal caller: invokes wrapped functions from this file (`sum_n`, `bump`)
// as well as ones defined in `strings.c` (`first_byte`, `strings_demo`).
int numbers_demo(void)
{
    int xs[3] = {1, 2, 3};
    int total = sum_n(xs, 3);
    total += bump(&xs[0]);
    total += (int)first_byte("world");
    return total + strings_demo();
}
