typedef struct {
    int scalar;
    int values[2];
} Container;

int direct_array[2];
Container record;
Container record_array[2];
int forced_mut_array[2];

int *direct_array_decay(void) {
    return direct_array;
}

Container *direct_address(void) {
    return &record;
}

int *member_address(void) {
    return &record.scalar;
}

int *member_array_decay(void) {
    return record.values;
}

Container *subscript_address(void) {
    return &record_array[1];
}

int *nested_subscript_address(void) {
    return &record_array[1].values[1];
}

int *forced_mut_array_decay(void) {
    return forced_mut_array;
}
