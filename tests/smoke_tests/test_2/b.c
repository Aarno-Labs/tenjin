int b_1()
{
  static int foo = 0;
  return foo++;
}

static int helper() { return 100; }

int b_help() { return helper(); }