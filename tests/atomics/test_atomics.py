import json
from pathlib import Path

import hermetic


def run_atomics(root: Path, tmp_codebase: Path, source_text: str) -> str:
    tmp_codebase.mkdir()
    source = tmp_codebase / "atomics.c"
    source.write_text(source_text, encoding="utf-8")
    clang = root / "_local" / "xj-llvm" / "bin" / "clang"
    resource_dir = (
        hermetic.run([clang, "-print-resource-dir"], check=True, capture_output=True)
        .stdout.decode()
        .strip()
    )
    (tmp_codebase / "compile_commands.json").write_text(
        json.dumps([
            {
                "directory": tmp_codebase.as_posix(),
                "file": source.as_posix(),
                "arguments": [
                    clang.as_posix(),
                    "-std=c11",
                    f"-resource-dir={resource_dir}",
                    "-c",
                    source.as_posix(),
                ],
            }
        ]),
        encoding="utf-8",
    )

    tool = root / "_local" / "_build_atomics" / "xj-prepare-atomics"
    hermetic.run(
        [tool, "--inplace", "-p", tmp_codebase, source],
        check=True,
        capture_output=True,
    )
    hermetic.run(
        [clang, "-std=c11", "-fsyntax-only", source],
        check=True,
        capture_output=True,
    )
    return source.read_text(encoding="utf-8")


def test_promotes_supported_scalar_globals_and_rewrites_accesses(root, tmp_codebase):
    transformed = run_atomics(
        root,
        tmp_codebase,
        """\
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

static _Bool ready;
static int8_t small = -1;
static uint64_t count;
static size_t total;
static ssize_t delta;
static int again;
static int again;

int update(int value) {
    ready = value != 0;
    small = 3;
    count += total + count;
    count++;
    int old = count++;
    total |= 8;
    delta ^= small;
    again = count;
    return ready ? old + count + again : (int)delta;
}
""",
    )

    assert "typedef _Atomic(_Bool) __tenjin_atomic_bool_t;" in transformed
    assert "typedef _Atomic(int8_t) __tenjin_atomic_i8_t;" in transformed
    assert "typedef _Atomic(uint64_t) __tenjin_atomic_u64_t;" in transformed
    assert "typedef _Atomic(size_t) __tenjin_atomic_usize_t;" in transformed
    assert "typedef _Atomic(ssize_t) __tenjin_atomic_isize_t;" in transformed
    assert "typedef _Atomic(int) __tenjin_atomic_i32_t;" in transformed
    assert "static __tenjin_atomic_bool_t ready;" in transformed
    assert "static __tenjin_atomic_i8_t small = -1;" in transformed
    assert "static __tenjin_atomic_u64_t count;" in transformed
    assert "static __tenjin_atomic_usize_t total;" in transformed
    assert "static __tenjin_atomic_isize_t delta;" in transformed
    assert transformed.count("static __tenjin_atomic_i32_t again;") == 2
    assert "__c11_atomic_store(&ready, value != 0, __ATOMIC_SEQ_CST);" in transformed
    assert "__c11_atomic_store(&small, 3, __ATOMIC_SEQ_CST);" in transformed
    assert (
        "__c11_atomic_fetch_add(&count, __c11_atomic_load(&total, __ATOMIC_SEQ_CST)"
        " + __c11_atomic_load(&count, __ATOMIC_SEQ_CST), __ATOMIC_SEQ_CST);" in transformed
    )
    assert "__c11_atomic_fetch_add(&count, 1, __ATOMIC_SEQ_CST);" in transformed
    assert "int old = __c11_atomic_fetch_add(&count, 1, __ATOMIC_SEQ_CST);" in transformed
    assert "__c11_atomic_fetch_or(&total, 8, __ATOMIC_SEQ_CST);" in transformed
    assert (
        "__c11_atomic_fetch_xor(&delta, __c11_atomic_load(&small, __ATOMIC_SEQ_CST),"
        " __ATOMIC_SEQ_CST);" in transformed
    )
    assert (
        "__c11_atomic_store(&again, __c11_atomic_load(&count, __ATOMIC_SEQ_CST),"
        " __ATOMIC_SEQ_CST);" in transformed
    )
    assert (
        "return __c11_atomic_load(&ready, __ATOMIC_SEQ_CST) ? old"
        " + __c11_atomic_load(&count, __ATOMIC_SEQ_CST)"
        " + __c11_atomic_load(&again, __ATOMIC_SEQ_CST)"
        " : (int)__c11_atomic_load(&delta, __ATOMIC_SEQ_CST);" in transformed
    )


def test_rejects_external_address_taken_nonscalar_and_unsupported_uses(root, tmp_codebase):
    source = """\
extern int external;
static int *pointer;
static int array[2];
static long double floating;
static unsigned __int128 too_wide;
static const int constant = 1;
static volatile int device;
static int address_taken;
static int multiplied;
static int assignment_value;
static int for_condition;
static int joined, joined_too;

static void consume(int *value) { (void)value; }
int use(void) {
    static int local_static;
    consume(&address_taken);
    multiplied *= 2;
    local_static++;
    for (; for_condition = 0;) {}
    pointer = array;
    return assignment_value = external + joined + joined_too + constant + device + floating
        + too_wide;
}
"""
    transformed = run_atomics(root, tmp_codebase, source)

    assert transformed == source


def test_rejects_a_macro_spelled_access_without_affecting_other_globals(root, tmp_codebase):
    transformed = run_atomics(
        root,
        tmp_codebase,
        """\
#define READ(value) (value)
static int hidden_by_macro;
static int ordinary;

int use(void) {
    ordinary = READ(hidden_by_macro);
    return ordinary;
}
""",
    )

    assert "static int hidden_by_macro;" in transformed
    assert "READ(hidden_by_macro)" in transformed
    assert "static __tenjin_atomic_i32_t ordinary;" in transformed
    assert "__c11_atomic_store(&ordinary, READ(hidden_by_macro), __ATOMIC_SEQ_CST);" in transformed
    assert "return __c11_atomic_load(&ordinary, __ATOMIC_SEQ_CST);" in transformed


def test_leaves_read_only_globals_untouched(root, tmp_codebase):
    transformed = run_atomics(
        root,
        tmp_codebase,
        """\
static int initialized_once = 7;
static int never_referenced;
static int updated;

int read(void) {
    return initialized_once;
}

void update(void) {
    updated++;
}
""",
    )

    assert "static int initialized_once = 7;" in transformed
    assert "static int never_referenced;" in transformed
    assert "return initialized_once;" in transformed
    assert "static __tenjin_atomic_i32_t updated;" in transformed
    assert "__c11_atomic_fetch_add(&updated, 1, __ATOMIC_SEQ_CST);" in transformed


def test_promotes_volatile_sig_atomic_t_but_not_other_volatile_integers(root, tmp_codebase):
    transformed = run_atomics(
        root,
        tmp_codebase,
        """\
#include <signal.h>

static volatile sig_atomic_t signal_flag;
static volatile int device_flag;

void handle_signal(int signal_number) {
    signal_flag = signal_number;
    device_flag = signal_number;
}

int observed_signal(void) {
    return signal_flag + device_flag;
}
""",
    )

    assert "typedef _Atomic(sig_atomic_t) __tenjin_atomic_i32_t;" in transformed
    assert "static volatile __tenjin_atomic_i32_t signal_flag;" in transformed
    assert "static volatile int device_flag;" in transformed
    assert "__c11_atomic_store(&signal_flag, signal_number, __ATOMIC_SEQ_CST);" in transformed
    assert "device_flag = signal_number;" in transformed
    assert "return __c11_atomic_load(&signal_flag, __ATOMIC_SEQ_CST) + device_flag;" in transformed
