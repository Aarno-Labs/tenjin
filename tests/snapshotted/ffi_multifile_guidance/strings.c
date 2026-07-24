// XREF:ffi_export_wrapper
// One of two source files in a multi-file codebase exercising `ffi` guidance.
// The `ffi` guidance keys are bare function names, so a wrapped function is
// selected the same way regardless of which file it lives in. See docs/USE.md.

#include "api.h"

unsigned char first_byte(const char *s)
{
    return (unsigned char)s[0];
}

void zero_first(unsigned char *buf, int n)
{
    if (n > 0)
    {
        buf[0] = 0;
    }
}

// Internal caller: invokes a wrapped function (`zero_first`) defined in this
// same file. The array argument is borrowed directly into the `&mut [u8]`
// parameter at the call site.
int strings_demo(void)
{
    unsigned char buf[4] = {9, 9, 9, 9};
    zero_first(buf, 4);
    return (int)buf[0];
}
