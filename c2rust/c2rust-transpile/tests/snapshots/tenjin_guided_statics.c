// xj-prepare-guidance output for statics guided as `String`, `&str` and
// `Vec<u8>`, with its header inlined. Owned values that need an allocation
// are assigned in `c2rust_run_static_initializers`.
typedef const char *xj_ty_ref_str;
typedef const char *xj_ty_String;
typedef unsigned char xj_ty_Vec_u8[4];
typedef unsigned char xj_ty_Vec_u8_1[8];
static inline char xj_char_at_String(xj_ty_String b, long i) { return b[i]; }
static inline char xj_char_at_ref_str(xj_ty_ref_str b, long i) { return b[i]; }
static inline _Bool xj_is_null_String(xj_ty_String p) { return p == 0; }

static xj_ty_String greeting = "hello";
static xj_ty_String nothing = 0;
static xj_ty_ref_str view = "view";
static xj_ty_Vec_u8 owned = {1, 2, 3, 4};
static xj_ty_Vec_u8_1 zeroed;

int use(int i) {
    static xj_ty_String local = "local";
    return xj_char_at_String(greeting, i) + (xj_is_null_String(nothing)) + xj_char_at_ref_str(view, i) + owned[i] + zeroed[i] + xj_char_at_String(local, i);
}
