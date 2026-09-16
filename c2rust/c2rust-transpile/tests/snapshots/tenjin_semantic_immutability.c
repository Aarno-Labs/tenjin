typedef struct {
    int value;
} Plain;

typedef struct {
    int *raw;
} HasRaw;

typedef struct {
    const unsigned char *guided;
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
const unsigned char *guided_pointer = "";
Plain plain_record;
HasRaw raw_record;
HasRawUnion raw_union;
AllGuided guided_record = {"guided"};
NestedRaw nested_raw_record_array;
NestedGuided nested_guided_record_array = {{{"first"}, {"second"}}};
int *raw_pointer_array[2];
int (*function_pointer)(int);
