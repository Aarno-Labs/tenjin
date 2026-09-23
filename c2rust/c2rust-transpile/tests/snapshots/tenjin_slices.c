// xj-prepare-guidance output for `x` guided as `&[u8]` in both functions,
// with its header inlined. `x++` on a shared slice is resliced.
typedef unsigned char *xj_ty_ref_slice_u8;
static inline unsigned char *xj_index_ref_slice_u8(xj_ty_ref_slice_u8 b, long i) { return (unsigned char *)(b + i); }
static inline xj_ty_ref_slice_u8 xj_slice_from_ref_slice_u8(unsigned char *b, long i) { return (xj_ty_ref_slice_u8)(b + i); }

// XREF:array_decay
void inc(xj_ty_ref_slice_u8 x)
{
    x = xj_slice_from_ref_slice_u8(x, 1);
}

unsigned char get(xj_ty_ref_slice_u8 x, int i)
{
    return (*xj_index_ref_slice_u8(x, i));
}
