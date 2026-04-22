#include <errno.h>

int fpointer(void (*f)(void))
{
	errno = 0;
	f();
	// This should fail the analysis
	if (errno == 1)
	{
		return -1;
	}
	return 0;
}

struct pointers
{
	void (*f)(void);
};

int struct_fpointer(struct pointers p)
{
	errno = 0;
	p.f();
	if (errno == 1)
	{
		return -1;
	}
	return 0;
}

int struct_ptr_fpointer(struct pointers *p)
{
	errno = 0;
	p->f();
	if (errno == 1)
	{
		return -1;
	}
	return 0;
}
