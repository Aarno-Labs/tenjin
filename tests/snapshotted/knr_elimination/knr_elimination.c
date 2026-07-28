#include <stdio.h>

/* (C) An unprototyped declaration, filled in from the definition below. */
extern int scale();

/* (A) An old-style definition. */
int scale(x, factor)
int x;
int factor;
{
    return x * factor;
}

/* (A) Promoted parameters. The caller passes `c` as an int and `weight` as a
   double, so the ABI-preserving rewrite keeps that signature and converts in
   the body rather than changing how the function is called. */
int blend(c, weight)
char c;
float weight;
{
    return (int)(c * weight);
}

/* (A) Joined declarators in the declaration list: clang reports `b`'s extent
   as the whole `int a, b`, so its type has to be rendered rather than copied. */
int total(a, b, label)
    int a, b;
    char *label;
{
    return a + b + label[0];
}

/* (A) An array parameter, which decays and so needs no ABI preservation. */
int first_of(buf, n)
    char buf[];
    int n;
{
    return buf[n];
}

/* (B) An unprototyped definition. */
int seven() { return 7; }

int main(void)
{
    printf("%d %d %d %d %d\n", scale(6, 7), blend('A', 2.0f), total(1, 2, "z"),
           first_of("xyz", 1), seven());
    return 0;
}
