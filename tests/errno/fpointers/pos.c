#include <errno.h>

int fpointer(void (*f)(void))
{
	f();	
	errno = 0;
	if (errno == 1) { return -1; }
	return 0;
}

struct pointers { 
    void (*f)(void);
};

int struct_fpointer(struct pointers p)
{
	p.f();
	errno = 0;
	if (errno == 1) { return -1; }
	return 0;
}

int struct_ptr_fpointer(struct pointers *p)
{
	p->f();
	errno = 0;
	if (errno == 1) { return -1; }
	return 0;
}
