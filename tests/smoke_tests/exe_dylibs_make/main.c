#include <stdio.h>

int lib();
int foo();

struct foo {};

int main()
{
  struct foo f;
  printf("Hello, Tenjin! %d %d\n", lib(), foo());
  return 0;
}
