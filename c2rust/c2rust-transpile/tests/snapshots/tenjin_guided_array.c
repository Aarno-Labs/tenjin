const char *const GUIDED_STRINGS[2] = {
    "zero",
    "one",
};

const char *first_guided_string(void) {
    return GUIDED_STRINGS[0];
}

int main(void) {
    return GUIDED_STRINGS[0][0] != 'z' || first_guided_string()[0] != 'z';
}
