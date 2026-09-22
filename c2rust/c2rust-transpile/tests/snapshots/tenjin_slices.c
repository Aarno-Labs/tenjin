// xj-prepare-guidance output for `x` guided as `&[u8]` in both functions,
// with its header inlined. `x++` on a shared slice is resliced.
typedef unsigned char *xj_ty_0;
static inline unsigned char *xj_index_0(xj_ty_0 b, long i) { return (unsigned char *)(b + i); }
static inline xj_ty_0 xj_slice_from_0(unsigned char *b, long i) { return (xj_ty_0)(b + i); }

// XREF:array_decay
void inc(xj_ty_0 x)
{
    x = xj_slice_from_0(x, 1);
}

unsigned char get(xj_ty_0 x, int i)
{
    return (*xj_index_0(x, i));
}
