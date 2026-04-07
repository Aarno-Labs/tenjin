#include <stdio.h>

int global = 42;
int __attribute__((noinline)) modifies_global() {
  return ++global;
}

int main()
{
  printf("Hello, Tenjin! %d\n", modifies_global());
  return 0;
}