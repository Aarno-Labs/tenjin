// xj-prepare-guidance output for statics guided as `String`, `&str` and
// `Vec<u8>`, with its header inlined. Owned values that need an allocation
// are assigned in `c2rust_run_static_initializers`. A record holding a guided
// `Vec` in place of an array, and an array of guided `String`s, default to
// constants, and records holding owned values by value are not `Copy`.
typedef const char *xj_ty_ref_str;
typedef const char *xj_ty_String;
typedef unsigned char xj_ty_Vec_u8[4];
typedef unsigned char xj_ty_Vec_u8_1[8];
typedef unsigned char xj_ty_Vec_u8_2[2];
typedef xj_ty_String xj_ty_array_3_String[3];
static inline char xj_char_at_String(xj_ty_String b, long i) { return b[i]; }
static inline char xj_char_at_ref_str(xj_ty_ref_str b, long i) { return b[i]; }
static inline _Bool xj_is_null_String(xj_ty_String p) { return p == 0; }

static xj_ty_String greeting = "hello";
static xj_ty_String nothing = 0;
static xj_ty_ref_str view = "view";
static xj_ty_Vec_u8 owned = {1, 2, 3, 4};
static xj_ty_Vec_u8_1 zeroed;
struct Rec { xj_ty_Vec_u8_2 buf; int n; };
struct Outer { struct Rec rec; };
static struct Outer outer;
static xj_ty_array_3_String names;

int use(int i) {
    static xj_ty_String local = "local";
    xj_ty_array_3_String local_names;
    return xj_char_at_String(greeting, i) + (xj_is_null_String(nothing)) + xj_char_at_ref_str(view, i) + owned[i] + zeroed[i] + xj_char_at_String(local, i)
        + outer.rec.buf[i] + outer.rec.n + (xj_is_null_String(names[i])) + (xj_is_null_String(local_names[i]));
}
