#include <errno.h>

int foo(void (*f)(void))
{
	errno = 0;
	f();	
	if (errno == 1) { return -1; }
	return 0;
}

int main(int argc, char**argv)
{
}
