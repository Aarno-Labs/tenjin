static char foo(int argc)
{
    char x_xjtr_1 = 'a';
    return (char)argc + x_xjtr_1;
}

static char foo_xjtr_0(int argc)
{
    char x = 'a';
    return (char)argc + x;
}

int main(int argc, char **argv) {
    foo(argc);
    foo_xjtr_0(argc);
    return 0;
}
