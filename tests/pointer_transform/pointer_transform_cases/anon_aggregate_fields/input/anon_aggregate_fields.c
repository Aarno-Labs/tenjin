#include <stdio.h>

/* Mirrors md4c's MD_MARK: an anonymous struct nested inside an
 * anonymous union.  Clang represents `m->beg` as three chained
 * MemberExprs sharing one source range (anon union -> anon struct ->
 * beg), so a rewriter that walks out only a single anonymous level
 * loses the field name and emits a dangling "marks[i].".
 *
 * `tag` covers the single-anonymous-level path alongside it, and `ch`
 * is an ordinary named field that needs no walking at all. */
typedef struct Mark_tag Mark;
struct Mark_tag {
    union {
        struct {
            unsigned beg;
            unsigned end;
        };
        void* pointer;      /* dummy marks can store a pointer instead */
    };
    union {
        unsigned tag;
    };
    char ch;
};

struct Ctx {
    Mark* marks;
    int n_marks;
};

static unsigned sum_spans(struct Ctx* ctx) {
    Mark* m;
    unsigned total = 0;

    for (m = ctx->marks; m < ctx->marks + ctx->n_marks; m++) {
        total += m->end - m->beg;   /* two anonymous levels */
        total += m->tag;            /* one anonymous level */
        total += (unsigned)m->ch;   /* named field, no levels */
    }
    return total;
}

/* Writes through the doubly-nested fields, so the ArrowWrite path is
 * exercised as well as the read path. */
static void widen(struct Ctx* ctx) {
    Mark* m;

    for (m = ctx->marks; m < ctx->marks + ctx->n_marks; m++) {
        m->end++;
        m->beg--;
    }
}

int main(void) {
    Mark marks[3];
    struct Ctx ctx;
    int i;

    for (i = 0; i < 3; i++) {
        marks[i].beg = (unsigned)(i + 1);
        marks[i].end = (unsigned)(i * 3 + 5);
        marks[i].tag = (unsigned)(i + 1);
        marks[i].ch = (char)('a' + i);
    }
    ctx.marks = marks;
    ctx.n_marks = 3;

    printf("%u\n", sum_spans(&ctx));
    widen(&ctx);
    printf("%u\n", sum_spans(&ctx));
    return 0;
}
