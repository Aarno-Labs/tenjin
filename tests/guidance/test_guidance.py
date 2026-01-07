import guidance


def parse_testcase(s, e1, e2, e3, e4):
    a, b, c, d = guidance.parse_decl_specifier(s)
    assert a == e1
    assert b == e2
    assert c == e3
    assert d == e4
    assert guidance.make_decl_specifier_str(a, b, c, d) == s


def test_parse_full():
    parse_testcase("f:x#13@a/b/c/d", "f", "x", "13", "a/b/c/d")


def test_parse_start_var():
    parse_testcase("*:v", "*", "v", None, None)


def test_parse_no_linum():
    parse_testcase("f:v@a/b/c/d", "f", "v", None, "a/b/c/d")


def test_parse_no_file():
    parse_testcase("f:v#13", "f", "v", "13", None)


def rename(x):
    if x == "me":
        return "rename"
    return x


def test_singleton():
    g = {"vars_of_type": {"t": "me:x"}, "vars_mut": {"me": True}, "fn_return_type": {"me": "t"}}
    guidance.map_function_names(g, rename)
    assert g["vars_of_type"]["t"] == "rename:x"
    assert g["vars_mut"]["rename"]
    assert g["fn_return_type"]["rename"] == "t"


def test_list():
    g = {"vars_of_type": {"t": ["me:x"]}}
    guidance.map_function_names(g, rename)
    assert g["vars_of_type"]["t"] == ["rename:x"]

    g = {"vars_of_type": {"t": ["foo:y", "me:x"]}}
    guidance.map_function_names(g, rename)
    assert g["vars_of_type"]["t"][1] == "rename:x"
