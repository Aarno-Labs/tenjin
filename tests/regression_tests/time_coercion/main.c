#include <time.h>

time_t wrap_time(time_t *tloc)
{
  return time(tloc);
}

int main()
{
  time_t t;
  (void)wrap_time(&t);
  return 0;
}