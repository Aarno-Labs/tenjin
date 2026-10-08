// A NULL parameter pointer is reseated to a local buffer; exits with 6.
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
	return write_not_null(0);
}
