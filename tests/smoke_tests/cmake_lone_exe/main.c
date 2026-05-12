#include <stdio.h>

int global = 42;
int __attribute__((noinline)) modifies_global() {
  return ++global;
}

_Static_assert(sizeof(unsigned) == 4, "unsigned is not 4 bytes!");

unsigned float_to_bits(float flt) {
    union {
        float flt;
        unsigned num;
    } in;
    in.flt = flt;
    return in.num;
}

int main()
{
  printf("Hello, Tenjin! %d\n", modifies_global());
  return 0;
}