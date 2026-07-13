// Regression fixture for the pointer-to-index transform.
//
// `dest` is a parameter pointer that is conditionally reseated to a
// local buffer (`if (!dest) dest = buf;`). The pointer-to-index rewrite
// must NOT turn this into an index off `dest`: doing so drops the reseat
// (rewriting `dest = buf` to a bare index reset) and dereferences the
// caller's NULL pointer. validatePointerCandidate rejects the pointer
// so it is left as a pointer here.
//
// See xj-prepare-pointertransform/ValidationMethods.cpp
// ("Parameter reseated to a different base array").
int write_not_null(int *dest)
{
	int buf[4] = {0};
	if (!dest) dest = buf;
	*dest++ = 1;
	*dest++ = 2;
	*dest++ = 3;

	return buf[0] + buf[1] + buf[2];
}

int main(void)
{
	// Passing NULL exercises the reseat: writes must land in `buf`, so
	// the sum is 1 + 2 + 3 = 6 rather than a NULL dereference.
	return write_not_null(0);
}
