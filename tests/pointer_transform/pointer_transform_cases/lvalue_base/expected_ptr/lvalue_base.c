#include <stdio.h>

/* The pointer's base is a struct field rather than a variable. */

struct table {
    char *storage;
    unsigned len;
};

static unsigned log_calls = 0;

static void log_start(void) { log_calls++; }

static unsigned checksum(struct table *t) {
    char *p = t->storage;
    int p_index_xj = 0;
    unsigned sum = 0;
    for (unsigned i = 0; i < t->len; i++)
        sum += (unsigned char)p[p_index_xj++];
    return sum;
}

/* A store to a sibling field does not invalidate the base. */
static unsigned checksum_and_reset(struct table *t) {
    char *p = t->storage;
    int p_index_xj = 0;
    unsigned sum = 0;
    for (unsigned i = 0; i < t->len; i++)
        sum += (unsigned char)p[p_index_xj++];
    t->len = 0;
    return sum;
}

/* A call that could modify the field invalidates the base. */
static unsigned checksum_logged(struct table *t) {
    char *p = t->storage;
    int p_index_xj = 0;
    unsigned sum = 0;
    log_start();
    for (unsigned i = 0; i < t->len; i++)
        sum += (unsigned char)p[p_index_xj++];
    return sum;
}

int main(void) {
    char store[4] = {1, 2, 3, 4};
    struct table t = {store, 4};
    printf("%u ", checksum(&t));
    printf("%u ", checksum_logged(&t));
    printf("%u ", checksum_and_reset(&t));
    printf("%u %u\n", t.len, log_calls);
    return 0;
}
