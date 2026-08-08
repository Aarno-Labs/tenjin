// Regression fixture for the pointer-to-index transform.
//
// `dest` is a parameter pointer that is conditionally reseated to a
// local buffer (`if (!dest) dest = buf;`). Collapsing it to an index off
// its incoming argument would be wrong twice over: the reseat would be
// rewritten to a bare index reset, losing the assignment, and the
// writes would land on the caller's NULL pointer.
//
// The pointer is therefore rewritten in *handle* mode — retained,
// frozen, and indexed by dest_index_xj — which keeps the assignment
// (`(dest = buf, dest_index_xj = 0)`) and leaves `!dest` testing the
// pointer. Passing NULL must still write into `buf`.
//
// See xj-prepare-pointertransform/ValidationMethods.cpp
// ("parameter reseated to a base other than its incoming argument").
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
