// To avoid cross-platform output differences from `printf` argument names,
// we'll just declare printf and other libc functions ourselves.
int printf(const char *fmt, ...);
long strlen(const char *s);
void *memset(void *s, int c, long n);

void print_owned_String(const char *ostr)
{
    // XREF:guided_cast_str_of_owned_string
    printf("%s\n", ostr);
}
// void print_shared_str(const char* rstr) { printf("%s\n", rstr); }
// void print_exclusive_str(const char* xstr) { printf("%s\n", xstr); }
void print_unguided_ptr(const char *ptr) { printf("%s\n", ptr); }

void print_shared_vec_u8(const char *rvu8)
{
    // XREF:guided_cast_str_of_shared_vec_u8
    printf("%s\n", rvu8);
}

void guided_str_init_lit()
{
    const char *ostr = "owned String";
    // const char* rstr = "shared &str";
    // const char* xstr = "exclusive &mut str";
    const char *uptr = "unguided pointer";
}

// XREF:guided_ret_type
const char *guided_ret_ostr()
{
    // XREF:snapshot_guided_ret_ostr
    return "";
}

// NULL translates to use DARWIN_NULL on Mac; keep this
// snapshot test platform-independent.

int guided_condition_string_null_check_neq(const char *ostr)
{
    // XREF:guided_condition_string_null_check_neq
    return (ostr != ((void *)0)) ? 2 : 5;
}

void guided_c_assignment_string_pop(char* ostr)
{
    // XREF:guided_c_assignment_string_pop
    ostr[strlen(ostr) - 1] = '\0';
}


// void guided_vec_memset_zero_nosizeof(char* ovu8)
// {
//     // XREF:guided_vec_memset_zero_nosizeof
//     memset(ovu8, 0, 3);
// }


void guided_vec_memset_zero_mulsizeof_ty(char* ovu8)
{
    // XREF:guided_vec_memset_zero_mulsizeof_ty
    memset(ovu8, 0, sizeof(char) * 3);
}

void guided_vec_memset_zero_mulsizeof_deref(char* ovu8)
{
    // XREF:guided_vec_memset_zero_mulsizeof_deref
    memset(ovu8, 0, sizeof(*ovu8) * 3);
}

// int guided_noncondition_string_null_check_neq(const char* ostr) {
//     // XREF:guided_noncondition_string_null_check_neq
//     return ostr != ((void*)0);
// }

// int guided_condition_string_null_check_implicit(const char* ostr) {
//     // XREF:guided_condition_string_null_check_implicit (match_bool)
//     return ostr ? 2 : -1;
// }

// void guided_str_ptr_to_owned(const char* ptr) {
//     // Initializing an owned String from an unguided pointer.
//     // const char* ostr = ptr;

//     // Currently, trying to pass unguided pointer to owned String
//     // will result in a type mismatch in the generated Rust code.
//     // print_owned_String(ptr);
// }

int guided_mut_ref_neq(const char *xstr, const char *xstr2)
{
    // XREF:guided_mut_ref_neq
    return xstr != xstr2;
}

int guided_1d_slice(int *x, int index)
{
    return x[index];
}

int guided_2d_slice(int **x2d, int i, int j)
{
    return x2d[i][j];
}

int guided_1d_vec(int *x, int index)
{
    return x[index];
}

int guided_2d_vec(int **x2d, int i, int j)
{
    return x2d[i][j];
}