// Regression fixture for the pointer-to-index transform.
//
// `dest` is a parameter pointer that is conditionally reseated to a
// local buffer (`if (!dest) dest = buf;`). The reseat has to survive:
// dropping it — as an earlier rewrite did, by turning `dest = buf` into a
// bare index reset — dereferences the caller's NULL pointer instead.
//
// The pointer is retained and carries a companion index, so the reseat is
// an ordinary (base, index) assignment: `(dest = buf, dest_index_xj = 0)`.
// The contract here is behavioural — the program must still exit 6 — so
// check the snapshot by running it, not by reading it.
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
