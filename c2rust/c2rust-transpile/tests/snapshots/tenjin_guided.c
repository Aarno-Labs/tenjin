// To avoid cross-platform output differences from `printf` argument names,
// we'll just declare printf and other libc functions ourselves.
int printf(const char *fmt, ...);
int snprintf(char* buf, unsigned long, const char *fmt, ...);
int sprintf(char* buf, const char *fmt, ...);
long strlen(const char *s);
void *memset(void *s, int c, long n);
unsigned long strcspn(const char *, const char *);
// These are macros in <ctype.h> but Tenjin can translate the un-expanded form.
int isalnum(int c);
char tolower(int c);

extern int extern_int_unguided;
// XREF:extern_var_nonmutbl
extern int extern_int_nonmutbl;
// XREF:static_var_nonmutbl
static int static_int_nonmutbl = 0;

void use_global_ints()
{
    static int static_local_nonmutbl = 0;
    extern_int_unguided = 5 + extern_int_nonmutbl + static_int_nonmutbl + static_local_nonmutbl;
}

// XREF:fn_parameter_guided
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

void sprint_into_mutref_vec_u8(const char *xvu8)
{
    // XREF:sprint_into_mutref_vec_u8
    snprintf(xvu8, 24, "%d\n", 42);
    sprintf(xvu8, "%d\n", 42);
}

void guided_str_init_lit()
{
    // XREF:guided_local_nonmut
    const char *ostr = "owned String";
    // const char* rstr = "shared &str";
    // const char* xstr = "exclusive &mut str";
    const char *uptr = "unguided pointer";
}

void guided_static()
{
    // XREF:guided_static_globals
    static int u8;
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

unsigned long guided_c_strlen(char* ostr)
{
    // XREF:guided_c_strlen
    return strlen(ostr);
}

int guided_isalnum() {
    // XREF:guided_isalnum
    return isalnum('A');
}


int guided_tolower() {
    // XREF:guided_tolower
    return tolower('A');
}

int guided_strcspn(const char* ostr, const char* delimiters) {
    // XREF:guided_strcspn
    return strcspn(ostr, delimiters);
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

void guided_local_int_as_char() {
    char unguided = (char) 65;
    // char oc = unguided; // fails because unguided is i8, not Rust char
    // XREF:guided_int_as_char
    char oc = 65;
}

// notyet:guided_ret_coerce_borrow
// unsigned char guided_ret_ru8(unsigned char x)
// {
//     return x;
// }

void takes_shared_str(const char* rstr) { (void) rstr; }
void takes_shared_u8(unsigned char* ru8) { (void) ru8; }

void guided_coerce_borrow_arg() {
    // XREF:guided_arg_coerce_borrow
    const char* ostr = guided_ret_ostr();
    takes_shared_str(ostr);

    // notyet:
    // (gets unneeded `.as_ref().unwrap()`)
    //takes_shared_str(guided_ret_ostr());
}

void unguided_coerce_asref(unsigned char* unguided) {
    // XREF:unguided_arg_coerce_asref
    takes_shared_u8(unguided);
}

void guided_string_zero_empty()
{
    // XREF:guided_string_zero_empty
    const char* ostr = 0;
}

struct StructWithMembersA {
    unsigned char* uptr;
    unsigned char zu8;
};

void struct_unguided_ptr_with_guided_members(struct StructWithMembersA* ug_ptr) {
    //takes_shared_str(gm_ptr->ostr); // fails due to lack of Copy
    //takes_shared_u8(&gm_ptr->zu8); // fails due to lack of AsRef<_>
    ug_ptr->uptr[0] = 42;
    ug_ptr->zu8 = 43;
}

void struct_guided_ptr_with_guided_members(struct StructWithMembersA* gm_ptr) {
    //takes_shared_str(gm_ptr->ostr); // fails due to lack of Copy
    //takes_shared_u8(&gm_ptr->zu8); // fails due to lack of AsRef<_>
    // XREF:struct_guided_ptr_with_guided_members
    gm_ptr->uptr[0] = 42;
    gm_ptr->zu8 = 43;
}