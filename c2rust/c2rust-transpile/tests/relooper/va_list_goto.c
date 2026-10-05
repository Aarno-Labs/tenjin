#include <stdarg.h>

// Reduced from howerj/lisp: a goto before va_list forces its declaration to be hoisted.
static int sum(int count, ...) {
    int value = 0;
    if (count == 0)
        goto end;

    va_list ap;
    va_start(ap, count);
    for (int i = 0; i < count; i++)
        value += va_arg(ap, int);
    va_end(ap);
end:
    return value;
}

int main(void) {
    return sum(0) != 0 || sum(3, 10, 20, 12) != 42 || sum(1, -7) != -7;
}
