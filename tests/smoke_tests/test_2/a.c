int a_1()
{
  static int foo = 0;
  return foo++;
}

int a_2()
{
  static int foo = 0;
  return foo++;
}

static int helper() { return 42; }

int a_help()
{
  return helper();
}