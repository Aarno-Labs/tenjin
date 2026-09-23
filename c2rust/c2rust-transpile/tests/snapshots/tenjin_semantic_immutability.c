// xj-prepare-guidance output for `guided_pointer` and `AllGuided::guided`
// guided as `&'static [u8]`, with its header inlined.
typedef const unsigned char *xj_ty_ref_slice_u8;

typedef struct {
    int value;
} Plain;

typedef struct {
    int *raw;
} HasRaw;

typedef struct {
    xj_ty_ref_slice_u8 guided;
} AllGuided;

typedef union {
    int scalar;
    int *raw;
} HasRawUnion;

typedef struct {
    HasRaw values[2];
} NestedRaw;

typedef struct {
    AllGuided values[2];
} NestedGuided;

int plain_scalar;
int forced_mut_scalar;
int *raw_pointer;
int *forced_immutable_raw_pointer;
xj_ty_ref_slice_u8 guided_pointer = "";
Plain plain_record;
HasRaw raw_record;
HasRawUnion raw_union;
AllGuided guided_record = {"guided"};
NestedRaw nested_raw_record_array;
NestedGuided nested_guided_record_array = {{{"first"}, {"second"}}};
int *raw_pointer_array[2];
int (*function_pointer)(int);
static const char *immutable_keywords[] = {"if", "else"};
unsigned long immutable_sizeof_subscript =
    sizeof(immutable_keywords) / sizeof(immutable_keywords[0]);
