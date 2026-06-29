// Output shape of the xj-prepare-allocmotion pass: a stack value is built and
// handed to the box__new_xjtr copy-into-heap helper. The transpiler rewrites
// the box__new_xjtr call into Box::into_raw(Box::new(v)).

extern void *malloc(__SIZE_TYPE__);
extern void *memcpy(void *, const void *, __SIZE_TYPE__);
static inline void *box__new_xjtr(const void *_xj_from, __SIZE_TYPE__ _xj_size) {
    void *_xj_p = malloc(_xj_size);
    if (_xj_p) memcpy(_xj_p, _xj_from, _xj_size);
    return _xj_p;
}

struct Point {
    int x;
    int y;
};

struct Point *make_point(int a, int b) {
    struct Point _xj_p_stack = { .x = a, .y = b };
    struct Point *p = box__new_xjtr(&_xj_p_stack, sizeof(_xj_p_stack));
    return p;
}
