// xj-prepare-guidance output for `GUIDED_STRINGS` guided as
// `[&'static [u8]; 2]`, with its header inlined.
typedef const char *xj_ty_ref_slice_u8;
typedef const xj_ty_ref_slice_u8 xj_ty_array_2_ref_slice_u8[2];
static inline const char *xj_index_ref_slice_u8(xj_ty_ref_slice_u8 b, long i) { return (const char *)(b + i); }
static inline const char *xj_slice_all_ptr_const_char(const char *b) { return (const char *)b; }

xj_ty_array_2_ref_slice_u8 GUIDED_STRINGS = {
    "zero",
    "one",
};

const char *first_guided_string(void) {
    return xj_slice_all_ptr_const_char(GUIDED_STRINGS[0]);
}

int main(void) {
    return (*xj_index_ref_slice_u8(GUIDED_STRINGS[0], 0)) != 'z' || first_guided_string()[0] != 'z';
}
