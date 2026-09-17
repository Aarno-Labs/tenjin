extern float next_float(void);

int classify_float(void) {
    return __builtin_fpclassify(10, 20, 30, 40, 50, next_float());
}

int classify_double(double value) {
    return __builtin_fpclassify(-1, -2, -3, -4, -5, value);
}
