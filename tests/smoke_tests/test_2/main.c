int a_1();
int a_2();
int b_1();
int a_help();
int b_help();

#include <stdio.h>

int main()
{
  printf("a_1 => %d\n", a_1());
  printf("a_1 => %d\n", a_1());
  printf("a_1 => %d\n", a_1());
  printf("a_2 => %d\n", a_2());
  printf("b_1 => %d\n", b_1());
  printf("b_1 => %d\n", b_1());
  printf("a_2 => %d\n", a_2());
  printf("a_help => %d\n", a_help());
  printf("b_help => %d\n", b_help());
  return 0;
}