import json
import os
import re

import pytest

import c_refact
import compilation_database
import hermetic
import llvm_bitcode_linking
import pangs_source
import repo_root
import translation_preparation


FOLDED_SOURCE = """
static int g=7;
static int discarded(void){return ++g;}
int main(void){
    __typeof__(discarded()) x=0;
    int a[sizeof(discarded())];
    enum { N=sizeof(discarded()) };
    struct Width { unsigned n:sizeof(discarded()); };
    int y __attribute__((aligned(sizeof(discarded()))))=0;
    return _Generic(g++, int: g, default: discarded()) + x + y + sizeof(a) + N
        + __builtin_types_compatible_p(__typeof__(discarded()), int);
}
"""


def contract(root, source, edits=None):
    edit_list = edits or []
    field = {
        "global": "g",
        "llvm_name": "g",
        "accessors": ["main"],
        "functions": ["main"],
        "rewrite_callsites": [],
        "blockers": [],
        "source_edits": edit_list,
    }
    return {
        "schema_version": 8,
        "run": {"analysis": {"repo_root": str(root)}},
        "globals": [
            {"key": "g", "meta": {"llvm_name": "g"}, "disposition": {"chosen": "localize"}}
        ],
        "context_rewrite": {
            "source": {
                "version": 3,
                "emitter": "tenjin-c2rust-default-v1",
                "retention": {
                    "policy": "c2rust-declaration-dependencies",
                    "version": 1,
                    "preserve_unused_functions": False,
                },
                "complete": True,
                "construction": "automatic-context-in-main",
                "globals_without_initializers": ["g"],
                "files": [{"path": source.name}],
            },
            "fields": [field],
            "selected": {"fields": [field], "functions": ["main"], "source_edits": edit_list},
        },
    }


def test_plan_rejects_legacy_conflicting_and_escaping_edits(tmp_path):
    source = tmp_path / "test.i"
    source.write_text("static int g; int main(void){return ++g;}")
    manifest = contract(tmp_path, source)
    pangs_source.validate_plan(manifest, tmp_path)
    manifest["context_rewrite"]["source"]["version"] = 0
    with pytest.raises(ValueError, match="source-complete"):
        pangs_source.validate_plan(manifest, tmp_path)
    with pytest.raises(ValueError, match="escapes"):
        pangs_source.snapshot_path(tmp_path, "../outside")
    edit = {
        "file": source.name,
        "start": 0,
        "end": 0,
        "replacement": "one",
        "kind": "signature",
    }
    manifest = contract(tmp_path, source, [edit, {**edit, "replacement": "two"}])
    with pytest.raises(ValueError, match="Conflicting"):
        pangs_source.validate_plan(manifest, tmp_path)


def test_plan_validation_needs_only_manifest_metadata(tmp_path):
    manifest = contract(tmp_path, tmp_path / "test.i")
    assert pangs_source.validate_plan(manifest, tmp_path) == manifest["context_rewrite"]["source"]


def test_plan_consumes_final_edits_without_interpreting_wrapper_recipes(tmp_path):
    source = tmp_path / "test.i"
    code = "int plain(void){return 2;}\n"
    source.write_text(code)
    manifest = contract(tmp_path, source)
    field = manifest["context_rewrite"]["fields"][0]
    # Candidate recipe formats belong to PANGS, not the edit consumer.
    field.pop("source_edits")
    field["source_wrappers"] = {"opaque_planning_metadata": True}
    selected = manifest["context_rewrite"]["selected"]
    wrapper = "int plain_xjw(struct XjGlobals *xjg){(void)xjg;return plain();}\n"
    selected["source_edits"] = [
        {
            "file": source.name,
            "start": len(code),
            "end": len(code),
            "replacement": wrapper,
            "kind": "wrapper-definition",
        }
    ]
    original_manifest = json.dumps(manifest, sort_keys=True)
    pangs_source.validate_plan(manifest, tmp_path)
    pangs_source.apply_source_edits(manifest, tmp_path)
    assert source.read_text() == "struct XjGlobals;\n" + code + wrapper
    assert json.dumps(manifest, sort_keys=True) == original_manifest


@pytest.mark.parametrize(
    "changes,match",
    [
        ({"file": "../outside.i"}, "escapes"),
        ({"file": "unlisted.i"}, "Unsupported PANGS source edit"),
        ({"kind": "unsupported-operation"}, "Unsupported PANGS source edit"),
    ],
)
def test_plan_checks_final_edit_paths_and_operations(tmp_path, changes, match):
    manifest = contract(tmp_path, tmp_path / "test.i")
    manifest["context_rewrite"]["selected"]["source_edits"] = [
        {
            "file": "test.i",
            "start": 0,
            "end": 0,
            "replacement": "",
            "kind": "signature",
            **changes,
        }
    ]
    with pytest.raises(ValueError, match=match):
        pangs_source.validate_plan(manifest, tmp_path)


@pytest.mark.parametrize("overlap", [False, True])
def test_plan_checks_final_edit_overlap_independently_of_list_order(tmp_path, overlap):
    manifest = contract(tmp_path, tmp_path / "test.i")
    edits = [
        {"file": "test.i", "start": start, "end": end, "replacement": "", "kind": "signature"}
        for start, end in [(4, 5), (0, 6 if overlap else 4)]
    ]
    manifest["context_rewrite"]["selected"]["source_edits"] = edits
    original_manifest = json.dumps(manifest, sort_keys=True)
    if overlap:
        with pytest.raises(ValueError, match="Conflicting"):
            pangs_source.validate_plan(manifest, tmp_path)
    else:
        pangs_source.validate_plan(manifest, tmp_path)
    assert json.dumps(manifest, sort_keys=True) == original_manifest


@pytest.mark.parametrize("start,end", [(-1, 0), (2, 1)])
def test_plan_rejects_invalid_edit_ranges(tmp_path, start, end):
    source = tmp_path / "test.i"
    edit = {
        "file": source.name,
        "start": start,
        "end": end,
        "replacement": "",
        "kind": "signature",
    }
    manifest = contract(tmp_path, source, [edit])
    with pytest.raises(ValueError, match="Invalid PANGS source edit range"):
        pangs_source.validate_plan(manifest, tmp_path)


def test_failed_materialization_leaves_original_tree_untouched(tmp_path, monkeypatch):
    root = tmp_path / "stage"
    root.mkdir()
    source = root / "test.i"
    code = "static int g; int main(void){return ++g;}"
    source.write_text(code)
    manifest_path = root / "manifest.json"
    manifest_path.write_text(json.dumps(contract(root, source)))
    compdb = compilation_database.synthetic_compile_commands_for_c_file(source, root)

    def fail(_manifest, _compdb, staged):
        (staged / "test.i").write_text("broken")
        raise ValueError("intentional materializer failure")

    monkeypatch.setattr(c_refact, "_localize_mutable_globals_in_place", fail)
    with pytest.raises(ValueError, match="intentional"):
        c_refact.localize_mutable_globals(manifest_path, compdb, root)
    assert source.read_text() == code
    assert list(tmp_path.iterdir()) == [root]


def test_guidance_forwards_exact_local_static_binding_without_source_policy(tmp_path):
    manifest_path = tmp_path / "manifest.json"
    manifest_path.write_text(
        json.dumps({
            "schema_version": 8,
            "globals": [
                {
                    "meta": {"llvm_name": "function.g"},
                    "disposition": {"chosen": "immutable"},
                    "facts": {
                        "source_obligations": {
                            "emitter": "tenjin-c2rust-default-v1",
                            "declaration": "function:g",
                            "observations": [{"kind": "address"}],
                        }
                    },
                }
            ],
        })
    )
    guidance = tmp_path / "guidance.json"
    guidance.write_text("{}")
    assert translation_preparation.add_immutable_dispositions_to_guidance(
        manifest_path, guidance
    ) == {"function:g"}
    assert json.loads(guidance.read_text()) == {
        "semantically_immutable_globals": ["function:g"],
        "vars_mut": {},
    }


@pytest.fixture
def source_pangs():
    if not os.environ.get("XJ_PANGS_EXE"):
        pytest.skip("set XJ_PANGS_EXE to a source-enabled PANGS build for cross-repo integration")
    return hermetic.xj_pangs_exe(repo_root.localdir())


def analyze(source_pangs, root, code, *, localize_only=True):
    compdb, manifest = analyze_sources(
        source_pangs, root, {"test.nolines.i": code}, localize_only=localize_only
    )
    return root / "test.nolines.i", compdb, manifest


def analyze_sources(source_pangs, root, sources, *, localize_only=True, leave_globals=()):
    """Analyze the same preprocessed C files that the materializer will rewrite."""
    root.mkdir()
    commands = []
    for name, code in sources.items():
        source = root / name
        source.parent.mkdir(parents=True, exist_ok=True)
        source.write_text(code)
        commands.extend(
            compilation_database.synthetic_compile_commands_for_c_file(source, root).commands
        )
    (root / "xj-guidance.json").write_text("{}")
    compdb = compilation_database.CompileCommands(commands)
    compdb.to_json_file(root / "compile_commands.json")
    bc = root / "linked_module.bc"
    database = root / "effective.json"
    llvm_bitcode_linking.compile_and_link_bitcode(
        compdb, bc, use_llvm14=True, source_compdb_path=database
    )
    overrides = root / "overrides.toml"
    overrides_text = '[cascade]\norder = ["localize"]\n' if localize_only else ""
    for key in leave_globals:
        overrides_text += f'\n[globals.{json.dumps(key)}]\ndisposition = "unhandled"\n'
    overrides.write_text(overrides_text)
    hermetic.run(
        [
            source_pangs,
            "analyze",
            bc,
            "--out",
            root / "pangs-disposition",
            "--repo-root",
            root,
            "--build-mode",
            "executable",
            "--dispose",
            "--manifest-only",
            "--source-compdb",
            database,
            "--overrides",
            overrides,
        ],
        check=True,
        capture_output=True,
        env_ext={"XJ_USE_LLVM14": "1"},
    )
    return compdb, root / "pangs-disposition" / "pangs-manifest.json"


def test_source_plan_materializes_indirect_mixed_targets_and_preserves_behavior(
    tmp_path, source_pangs
):
    root = tmp_path / "project"
    code = (
        "static int g;\n"
        "static int needs(void){return ++g;}\n"
        "static int ordinary(void){return 2;}\n"
        "typedef int (*Callback)(void);\n"
        "struct Ops { Callback fn; };\n"
        "static int dead(struct Ops *p){ return p->fn(); }\n"
        "int main(int argc, char **argv){\n"
        "  struct Ops op = {argc > 1 ? needs : ordinary};\n"
        "  if(0) return dead(&op);\n"
        "  return op.fn() + needs();\n"
        "}\n"
    )
    source, compdb, manifest_path = analyze(source_pangs, root, code)
    manifest = json.loads(manifest_path.read_text())
    assert manifest["context_rewrite"]["selected"]["fields"], manifest
    executable = tmp_path / "before"
    hermetic.run(["clang", "-x", "c", source, "-o", executable], check=True, capture_output=True)
    before = [hermetic.run([executable, *args], check=False).returncode for args in ([], ["arg"])]
    c_refact.localize_mutable_globals(manifest_path, compdb, root)
    rewritten = source.read_text()
    assert "Callback__pangs_context" in rewritten
    assert "dead(struct XjGlobals *xjg" in rewritten
    assert "p->fn(xjg)" in rewritten
    assert "ordinary(void)" in rewritten
    assert "ordinary_xjw(struct XjGlobals *xjg)" in rewritten
    assert c_refact.XJG_PLACEHOLDER not in rewritten
    executable = tmp_path / "after"
    hermetic.run(["clang", "-x", "c", source, "-o", executable], check=True, capture_output=True)
    after = [hermetic.run([executable, *args], check=False).returncode for args in ([], ["arg"])]
    assert before == after == [3, 3]
    materialized = json.loads(manifest_path.read_text())
    assert materialized["materialization"]["c_validation"] == ["clang-14", "clang-21"]
    assert materialized["context_rewrite"]["fields"] == manifest["context_rewrite"]["fields"]


def materialize_sources(source_pangs, tmp_path, sources, *, leave_globals=()):
    """Check a real localization plan against C execution and Rust compilation."""
    root = tmp_path / "project"
    compdb, path = analyze_sources(source_pangs, root, sources, leave_globals=leave_globals)
    manifest = json.loads(path.read_text())
    global_ = next(g for g in manifest["globals"] if g["meta"]["llvm_name"] == "g")
    assert global_["disposition"]["chosen"] == "localize", global_

    def run_c(name):
        executable = tmp_path / name
        hermetic.run(
            ["clang", "-x", "c", *compdb.get_source_files(), "-o", executable],
            check=True,
            capture_output=True,
        )
        return [hermetic.run([executable, *args], check=False).returncode for args in ([], ["arg"])]

    before = run_c("before")
    c_refact.localize_mutable_globals(path, compdb, root)
    rewritten = {name: (root / name).read_text() for name in sources}
    assert all(c_refact.XJG_PLACEHOLDER not in text for text in rewritten.values())
    assert run_c("after") == before
    assert json.loads(path.read_text())["globals"] == manifest["globals"]
    rust = tmp_path / "rust"
    hermetic.run(
        [
            repo_root.find_repo_root_dir_Path()
            / "c2rust"
            / "target"
            / os.environ.get("XJ_BUILD_RS_PROFILE", "debug")
            / "c2rust",
            "transpile",
            root / "compile_commands.json",
            "-o",
            rust,
            "--emit-build-files",
            "--disable-refactoring",
            "--guidance",
            root / "xj-guidance.json",
        ],
        check=True,
        capture_output=True,
    )
    assert any("XjGlobals" in source.read_text() for source in rust.rglob("*.rs"))
    hermetic.run(
        ["cargo", "check", "--manifest-path", rust / "Cargo.toml"],
        cwd=rust,
        check=True,
        capture_output=True,
    )
    return manifest, rewritten


def test_callback_global_declaration_and_definition_change_together(tmp_path, source_pangs):
    _, rewritten = materialize_sources(
        source_pangs,
        tmp_path,
        {
            "a.nolines.i": "extern int (*dispatch)(int);\nint main(void){return dispatch(7);}\n",
            "b.nolines.i": "int g;\nint foo(int x){return ++g+x;}\nint (*dispatch)(int)=foo;\n",
        },
        leave_globals=("b.nolines.i::dispatch",),
    )
    for text in rewritten.values():
        assert "(*dispatch)(struct XjGlobals *, int)" in text


def test_same_named_local_callbacks_remain_independent_across_files(tmp_path, source_pangs):
    manifest, rewritten = materialize_sources(
        source_pangs,
        tmp_path,
        {
            "a.nolines.i": "static int g;\nint needs_globals(int x){return ++g+x;}\n"
            "int use_needs_globals(int x){int (*fp)(int)=needs_globals;return fp(x);}\n"
            "extern int use_stays_plain(int);\n"
            "int main(void){return use_needs_globals(3)+use_stays_plain(2);}\n",
            "b.nolines.i": "int stays_plain(int x){return x-1;}\n"
            "int use_stays_plain(int x){int (*fp)(int)=stays_plain;return fp(x);}\n",
        },
    )
    assert "(*fp)(struct XjGlobals *, int)" in rewritten["a.nolines.i"]
    assert "(*fp)(int)=stays_plain" in rewritten["b.nolines.i"]
    assert "stays_plain" not in manifest["context_rewrite"]["selected"]["functions"]


def test_callback_typedef_field_changes_in_every_file(tmp_path, source_pangs):
    declarations = "typedef int (*callback_t)(int);\nstruct Holder {callback_t cb;};\n"
    _, rewritten = materialize_sources(
        source_pangs,
        tmp_path,
        {
            "a.nolines.i": declarations + "int g;\nint foo(int x){return ++g+x;}\n"
            "struct Holder holder={foo};\n",
            "b.nolines.i": declarations + "extern struct Holder holder;\n"
            "int main(void){return holder.cb(7);}\n",
        },
        leave_globals=("a.nolines.i::holder",),
    )
    for text in rewritten.values():
        assert "typedef int (*callback_t)(int);" in text
        assert "typedef int (*callback_t__pangs_context)(struct XjGlobals *, int);" in text
        assert "struct Holder {callback_t__pangs_context cb;};" in text


@pytest.mark.parametrize("flow", ["parameter", "parameter-to-field", "field-to-argument"])
def test_callback_typedef_flow_updates_redeclarations(tmp_path, source_pangs, flow):
    if flow == "parameter":
        body = "return cb(x);"
        use = "return apply(foo,1)+apply(bar,2);"
    elif flow == "parameter-to-field":
        body = "struct Holder holder;holder.cb=cb;return holder.cb(x);"
        use = "return apply(foo,1)+apply(bar,2);"
    else:
        body = "return cb(x);"
        use = "struct Holder holder={bar};return apply(foo,1)+apply(holder.cb,2);"
    _, rewritten = materialize_sources(
        source_pangs,
        tmp_path,
        {
            "test.nolines.i": "static int g;\n"
            "typedef int (*callback_t)(int);\nstruct Holder {callback_t cb;};\n"
            "int apply(callback_t cb,int x);\n"
            "int foo(int x){return ++g+x;}\nint bar(int x){return x+2;}\n"
            f"int apply(callback_t cb,int x){{{body}}}\n"
            f"int main(void){{{use}}}\n",
        },
    )
    text = rewritten["test.nolines.i"]
    assert text.count("apply(struct XjGlobals *xjg, callback_t__pangs_context cb,int x)") == 2
    assert "typedef int (*callback_t)(int);" in text
    if flow != "parameter":
        assert "struct Holder {callback_t__pangs_context cb;};" in text


@pytest.mark.parametrize("address", ["", "&"], ids=["implicit-address", "explicit-address"])
def test_mixed_callbacks_adapt_values_without_changing_original_function(
    tmp_path, source_pangs, address
):
    manifest, rewritten = materialize_sources(
        source_pangs,
        tmp_path,
        {
            "test.nolines.i": "static int g;\nint bar(int x);\n"
            "int foo(int x){return ++g+x;}\n"
            "int apply(int (*cb)(int),int x){return cb(x);}\n"
            f"int before(void){{return apply({address}foo,1)+apply({address}bar,2);}}\n"
            "int bar(int x){return x+2;}\n"
            f"int after(void){{return apply({address}bar,3);}}\n"
            "int main(void){return before()+after();}\n",
        },
    )
    assert {"foo", "apply", "before", "after"} <= set(
        manifest["context_rewrite"]["selected"]["functions"]
    )
    assert "bar" not in manifest["context_rewrite"]["selected"]["functions"]
    text = rewritten["test.nolines.i"]
    assert text.count("bar(int x)") == 2
    assert "bar_xjw(struct XjGlobals *xjg, int _xjw_arg_0)" in text
    assert f"apply(xjg, {address}bar_xjw,2)" in text
    assert "(*cb)(struct XjGlobals *, int)" in text


def test_adapters_leave_direct_calls_and_unrelated_callback_slots_unchanged(tmp_path, source_pangs):
    manifest, rewritten = materialize_sources(
        source_pangs,
        tmp_path,
        {
            "test.nolines.i": "static int g;\n"
            "int needs(int x){return ++g+x;}\nint plain(int x){return x+3;}\n"
            "int direct(void){return plain(2);}\n"
            "int separate(void){int (*q)(int)=plain;return q(3);}\n"
            "int main(int argc,char **argv){int (*p)(int)=argc>1?needs:plain;"
            "return p(1)+direct()+separate()+(p==plain);}\n",
        },
    )
    functions = set(manifest["context_rewrite"]["selected"]["functions"])
    assert not functions & {"plain", "direct", "separate"}
    text = rewritten["test.nolines.i"]
    assert "direct(void){return plain(2);}" in text
    assert "int (*q)(int)=plain;return q(3);" in text
    assert "argc>1?needs:plain_xjw" in text
    assert "(p==plain_xjw)" in text


def test_retained_external_callback_producer_can_be_adapted(tmp_path, source_pangs):
    manifest, rewritten = materialize_sources(
        source_pangs,
        tmp_path,
        {
            "test.nolines.i": "extern int abs(int);\nstatic int g;\n"
            "int needs(int x){return ++g+x;}\n"
            "int main(void){int (*p)(int)=needs;if(0)p=abs;"
            "return p(-2)+abs(-3);}\n",
        },
    )
    assert "abs" not in manifest["context_rewrite"]["selected"]["functions"]
    text = rewritten["test.nolines.i"]
    assert "extern int abs(int);" in text
    assert "if(0)p=abs_xjw" in text
    assert "return abs(_xjw_arg_0);" in text
    assert "+abs(-3);" in text


def test_multiple_wrappers_share_a_definition_insertion(tmp_path, source_pangs):
    manifest, rewritten = materialize_sources(
        source_pangs,
        tmp_path,
        {
            "test.nolines.i": "static int g;\n"
            "int needs(int x){return ++g+x;}\nint first(int x){return x+2;}\n"
            "int second(int x){return x+3;}\n"
            "int main(void){int (*p[3])(int)={needs,first,second};"
            "return p[0](1)+p[1](2)+p[2](3);}\n"
        },
    )
    assert not set(manifest["context_rewrite"]["selected"]["functions"]) & {"first", "second"}
    text = rewritten["test.nolines.i"]
    assert "{needs,first_xjw,second_xjw}" in text
    assert text.count("return first(_xjw_arg_0);") == 1
    assert text.count("return second(_xjw_arg_0);") == 1


@pytest.mark.parametrize("localize_h", [False, True])
def test_wrapper_selection_composes_across_localized_globals(tmp_path, source_pangs, localize_h):
    manifest, rewritten = materialize_sources(
        source_pangs,
        tmp_path,
        {
            "test.nolines.i": "static int g;\nstatic int h;\n"
            "int first(int x){return ++g+x;}\nint second(int x){return ++h+x;}\n"
            "int main(int argc,char **argv){int (*p)(int)=argc>1?first:second;return p(2);}\n",
        },
        leave_globals=() if localize_h else ("test.nolines.i::h",),
    )
    text = rewritten["test.nolines.i"]
    assert ("second" in manifest["context_rewrite"]["selected"]["functions"]) == localize_h
    if localize_h:
        assert "_xjw" not in text
        assert "second(struct XjGlobals *xjg, int x)" in text
    else:
        assert "second(int x)" in text
        assert "second_xjw(struct XjGlobals *xjg, int _xjw_arg_0)" in text


def test_wrapper_identity_is_shared_across_translation_units(tmp_path, source_pangs):
    shared = "struct Ops {int (*cb)(int);};\nint plain(int);\n"
    manifest, rewritten = materialize_sources(
        source_pangs,
        tmp_path,
        {
            "a.nolines.i": shared + "int g;\nint needs(int x){return ++g+x;}\n"
            "struct Ops a={plain};\nstruct Ops changed={needs};\n"
            "extern struct Ops b;\nint main(void){return changed.cb(1)+(a.cb!=b.cb);}\n",
            "b.nolines.i": shared + "int plain(int x){return x+3;}\nstruct Ops b={plain};\n",
        },
        leave_globals=("a.nolines.i::a", "a.nolines.i::changed", "b.nolines.i::b"),
    )
    assert "plain" not in manifest["context_rewrite"]["selected"]["functions"]
    assert all("plain_xjw(struct XjGlobals *xjg, int _xjw_arg_0);" in t for t in rewritten.values())
    assert sum(t.count("return plain(_xjw_arg_0);") for t in rewritten.values()) == 1


@pytest.mark.parametrize(
    "declarations,body,expected",
    [
        (
            "static void needs(int *p){*p=++g;}\nstatic void plain(int *p){*p=2;}\n",
            "int x=0;void (*p)(int *)=argc>1?needs:plain;p(&x);return x;",
            "plain(_xjw_arg_0);",
        ),
        (
            "typedef const int *Input;\nstatic Input plain(Input);\n"
            "static Input needs(Input p){++g;return p;}\nstatic Input plain(Input p){return p;}\n",
            "int x=3;Input (*p)(Input)=argc>1?needs:plain;return *p(&x);",
            "return plain(_xjw_arg_0);",
        ),
    ],
    ids=["void-return", "pointer-return-and-typedef-parameters"],
)
def test_wrapper_signatures_and_forwarding(tmp_path, source_pangs, declarations, body, expected):
    _, rewritten = materialize_sources(
        source_pangs,
        tmp_path,
        {
            "test.nolines.i": "static int g;\n"
            + declarations
            + f"int main(int argc,char **argv){{{body}}}\n"
        },
    )
    assert expected in rewritten["test.nolines.i"]


@pytest.mark.parametrize("storage", ["variable", "aggregate"])
def test_callback_addresses_in_initializers_and_assignments(tmp_path, source_pangs, storage):
    if storage == "variable":
        declarations = "int (*fp)(int)=&foo;\nvoid switch_callback(void){fp=&bar;}\n"
        body = "int first=fp(1);switch_callback();return first+fp(2);"
        expected_type = "(*fp)(struct XjGlobals *, int)"
        leave_globals = ("test.nolines.i::fp",)
    else:
        declarations = "struct Holder {int (*cb)(int);};\n"
        declarations += "struct Holder mod={&foo};\nstruct Holder unmod={&bar};\n"
        body = "return mod.cb(1)+unmod.cb(2);"
        expected_type = "(*cb)(struct XjGlobals *, int)"
        leave_globals = ("test.nolines.i::mod", "test.nolines.i::unmod")
    _, rewritten = materialize_sources(
        source_pangs,
        tmp_path,
        {
            "test.nolines.i": "static int g;\nint foo(int x){return ++g+x;}\n"
            "int bar(int x){return x+2;}\n" + declarations + f"int main(void){{{body}}}\n",
        },
        leave_globals=leave_globals,
    )
    assert expected_type in rewritten["test.nolines.i"]
    assert "bar(int x)" in rewritten["test.nolines.i"]
    assert "bar_xjw(struct XjGlobals *xjg, int _xjw_arg_0)" in rewritten["test.nolines.i"]


@pytest.mark.parametrize(
    "sources",
    [
        pytest.param(
            {
                "a.nolines.i": "extern int (*dispatch)(int);\n"
                "int main(void){return dispatch(7);}\n",
                "b.nolines.i": "int g;\nint foo(int x){return ++g+x;}\nint (*dispatch)(int)=foo;\n",
            },
            id="initializer-function-declared-in-another-file",
        ),
        pytest.param(
            {
                "test.nolines.i": "static int g;\nint foo(int x){return ++g+x;}\n"
                "struct Holder {int (*cb)(int);};\nstruct Holder holder={foo};\n"
                "int main(void){return holder.cb(7);}\n",
            },
            id="context-field-type-declared-after-first-accessor",
        ),
    ],
)
def test_localizing_callback_container_storage(tmp_path, source_pangs, sources):
    manifest, rewritten = materialize_sources(source_pangs, tmp_path, sources)
    assert all(g["disposition"]["chosen"] == "localize" for g in manifest["globals"])
    text = "\n".join(rewritten.values())
    assert "struct XjGlobals xjgv" in text
    if "a.nolines.i" in rewritten:
        assert "int foo(struct XjGlobals *xjg, int x);" in rewritten["a.nolines.i"]
    else:
        assert text.count("struct Holder {") == 1
        assert text.index("struct Holder {") < text.index('#include "xj_globals.h"')


@pytest.mark.parametrize(
    "declarations,field_type",
    [
        ("struct Holder {int (*cb)(int);};", "struct Holder"),
        ("typedef struct Holder {int (*cb)(int);} Holder;", "Holder"),
        (
            "typedef int (*Callback)(int);\n"
            "struct Base {int value;};\n"
            "typedef struct Base Base;\n"
            "struct Holder {Callback cb; Base base;};",
            "struct Holder",
        ),
        (
            "enum State {READY};\nstruct Holder {int (*cb)(int); enum State state;};",
            "struct Holder",
        ),
        (
            "union Value {int x; long y;};\n"
            "struct Holder {int (*cb)(int); union Value value; union Value *p;};",
            "struct Holder",
        ),
        (
            "struct Tail;\n"
            "struct Holder {int (*cb)(int); struct Tail *tail;};\n"
            "struct Tail {struct Holder holder;};",
            "struct Holder",
        ),
    ],
    ids=[
        "forward-tag",
        "embedded-typedef",
        "nested-types",
        "enum-field",
        "union-field",
        "pointer-cycle",
    ],
)
def test_context_storage_hoists_late_type_dependencies(
    tmp_path, source_pangs, declarations, field_type
):
    _, rewritten = materialize_sources(
        source_pangs,
        tmp_path,
        {
            "test.nolines.i": "struct Holder;\nstatic int g;\n"
            "static int foo(int x){return ++g+x;}\n"
            + declarations
            + f"\n{field_type} holder={{foo}};\n"
            "int main(void){return holder.cb(7);}\n"
        },
    )
    text = rewritten["test.nolines.i"]
    assert text.count("struct Holder {") == 1
    assert text.index("struct Holder {") < text.index('#include "xj_globals.h"')
    assert "static int foo(struct XjGlobals *xjg, int x)" in text


def test_callback_initializer_prototype_preserves_qualifiers_and_typedefs(tmp_path, source_pangs):
    _, rewritten = materialize_sources(
        source_pangs,
        tmp_path,
        {
            "a.nolines.i": "extern int (*dispatch)(const int *);\n"
            "int main(void){int x=7;return dispatch(&x);}\n",
            "b.nolines.i": "typedef const int *Input;\nint g;\n"
            "int foo(Input x){return ++g+*x;}\nint (*dispatch)(const int *)=foo;\n",
        },
    )
    text = rewritten["a.nolines.i"]
    assert "typedef const int *Input;" in text
    assert "int foo(struct XjGlobals *xjg, Input x);" in text


def test_hoisting_embedded_tag_keeps_unselected_storage(tmp_path, source_pangs):
    _, rewritten = materialize_sources(
        source_pangs,
        tmp_path,
        {
            "test.nolines.i": "static int g;\nint foo(int x){return ++g+x;}\n"
            "struct Holder {int (*cb)(int);} spare;\nstruct Holder holder={foo};\n"
            "int main(void){spare.cb=foo;return holder.cb(7)+spare.cb(1);}\n"
        },
        leave_globals=("test.nolines.i::spare",),
    )
    text = rewritten["test.nolines.i"]
    assert text.count("struct Holder {") == 1
    assert "struct Holder spare;" in text


def test_private_callback_initializer_stays_in_its_translation_unit(tmp_path, source_pangs):
    manifest, rewritten = materialize_sources(
        source_pangs,
        tmp_path,
        {
            "a.nolines.i": "extern int (*dispatch)(int);\nint main(void){return dispatch(7);}\n",
            "b.nolines.i": "int g;\nstatic int foo(int x){return ++g+x;}\n"
            "int (*dispatch)(int)=foo;\n",
        },
    )
    dispatch = next(g for g in manifest["globals"] if g["meta"]["llvm_name"] == "dispatch")
    assert dispatch["disposition"]["chosen"] == "unhandled"
    field = next(f for f in manifest["context_rewrite"]["fields"] if f["llvm_name"] == "dispatch")
    assert "source-private-initializer-function" in {b["kind"] for b in field["blockers"]}
    assert "foo" not in rewritten["a.nolines.i"]
    assert "static int foo(struct XjGlobals *xjg, int x)" in rewritten["b.nolines.i"]


def test_context_caller_does_not_change_unrelated_direct_or_indirect_callees(
    tmp_path, source_pangs
):
    manifest, rewritten = materialize_sources(
        source_pangs,
        tmp_path,
        {
            "test.nolines.i": "static int g;\nint target(int x){return x+1;}\n"
            "int tissue(int (*f)(int),int x){++g;return x?f(x):target(x);}\n"
            "int caller(void){return tissue(&target,((target)(1)));}\n"
            "int main(void){return caller();}\n",
        },
    )
    assert "target" not in manifest["context_rewrite"]["selected"]["functions"]
    text = rewritten["test.nolines.i"]
    assert "tissue(struct XjGlobals *xjg, int (*f)(int),int x)" in text
    assert "tissue(xjg, &target,((target)(1)))" in text
    assert "return x?f(x):target(x);" in text


@pytest.mark.parametrize("leaf", ["return ++g+x;", "return g+x;"], ids=["writer", "reader"])
def test_source_only_call_chain_and_global_users_are_planned(tmp_path, source_pangs, leaf):
    manifest, rewritten = materialize_sources(
        source_pangs,
        tmp_path,
        {
            "demo/test.nolines.i": "static int g;\n"
            f"static int dead_leaf(int x){{{leaf}}}\n"
            "static int dead_middle(int x){return dead_leaf(x);}\n"
            "static int dead_outer(int x){return dead_middle(x);}\n"
            'int main(void){if(sizeof("DEAD_C")==1u)return dead_outer(1);return ++g;}\n',
        },
    )
    assert {"dead_leaf", "dead_middle", "dead_outer"} <= set(
        manifest["context_rewrite"]["selected"]["functions"]
    )
    text = rewritten["demo/test.nolines.i"]
    for name in ("dead_leaf", "dead_middle", "dead_outer"):
        assert f"{name}(struct XjGlobals *xjg, int x)" in text
    assert '#include "../xj_globals.h"' in text
    assert "int main(void)" in text


@pytest.mark.parametrize(
    "extra,body,blocker",
    [
        (
            "typedef int (*callback_t)(int);\nint bar(int x){return x+2;}\n"
            "int apply(callback_t cb){return cb(1);}\n",
            "return apply(foo)+apply((callback_t)bar);",
            "source-callable-cast",
        ),
        (
            "int (*choose(void))(int){return foo;}\n",
            "return choose()(1);",
            "source-callable-return-type",
        ),
        (
            "extern int apply(int (*cb)(int));\n",
            "return apply(foo);",
            "source-external-callback:apply",
        ),
    ],
    ids=["explicit-cast", "returned-callback", "external-callback"],
)
def test_unsupported_callback_forms_are_rejected_before_materialization(
    tmp_path, source_pangs, extra, body, blocker
):
    root = tmp_path / "project"
    code = "static int g;\nint foo(int x){return ++g+x;}\n" + extra
    code += f"int main(void){{{body}}}\n"
    source, compdb, path = analyze(source_pangs, root, code)
    manifest = json.loads(path.read_text())
    global_ = next(g for g in manifest["globals"] if g["meta"]["llvm_name"] == "g")
    assert global_["disposition"]["chosen"] == "unhandled", global_
    field = next(f for f in manifest["context_rewrite"]["fields"] if f["llvm_name"] == "g")
    assert blocker in {b["kind"] for b in field["blockers"]}, field
    assert not manifest["context_rewrite"]["selected"]["fields"]
    c_refact.localize_mutable_globals(path, compdb, root)
    assert source.read_text() == code
    assert json.loads(path.read_text()) == manifest


def test_source_retention_matches_c2rust_declaration_pruning(tmp_path, source_pangs):
    root = tmp_path / "project"
    _, _, path = analyze(
        source_pangs,
        root,
        """
static int g;
static int drop_leaf(void){return ++g;}
static int drop_root(void){return drop_leaf();}
static inline int drop_inline(void){return ++g;}
static int keep_dead_branch(void){return ++g;}
static void keep_cleanup(int *p){if(0) ++g;}
static int keep_callback(void){return g;}
int (*exported_slot)(void)=keep_callback;
__attribute__((used)) static int keep_used(void){return g;}
int keep_exported(void){return g;}
int main(void){int x __attribute__((cleanup(keep_cleanup)))=0;
    if(0) return keep_dead_branch(); return g;}
""",
    )
    manifest = json.loads(path.read_text())
    rust = tmp_path / "rust"
    hermetic.run(
        [
            repo_root.find_repo_root_dir_Path()
            / "c2rust"
            / "target"
            / os.environ.get("XJ_BUILD_RS_PROFILE", "debug")
            / "c2rust",
            "transpile",
            root / "compile_commands.json",
            "-o",
            rust,
            "--emit-build-files",
            "--disable-refactoring",
        ],
        check=True,
        capture_output=True,
    )
    emitted = set()
    for source in rust.rglob("*.rs"):
        emitted.update(re.findall(r"\bfn\s+((?:keep|drop)_\w+)\s*\(", source.read_text()))
    expected = {"keep_dead_branch", "keep_cleanup", "keep_callback", "keep_used", "keep_exported"}
    assert emitted == expected
    assert set(manifest["context_rewrite"]["source"]["retained_functions"]) - {"main"} == emitted


def test_localization_prunes_discarded_source_writers_before_c_validation(tmp_path, source_pangs):
    root = tmp_path / "project"
    source, compdb, path = analyze(
        source_pangs,
        root,
        """
typedef struct { int unused; } Unused;
typedef int register_t __attribute__((__mode__(__word__)));
extern void harmless_header_declaration(Unused *);
typedef struct Retained { int x; } UnusedAlias;
static int g;
typedef __typeof__(g) DiscardedType;
extern DiscardedType discarded_prototype(void);
static int f(void){return ++g;}
static int (*unused_slot)(void)=f;
static int discarded(void){g=9; return unused_slot();}
int main(void){struct Retained r={0};
    __typeof__(discarded()) unused_type=0;
    int unused_bound[sizeof(discarded())];
    return _Generic(discarded(), int: _Generic(0, int: f, default: discarded),
                    default: discarded)()+r.x;}
""",
    )
    manifest = json.loads(path.read_text())
    assert manifest["context_rewrite"]["selected"]["fields"], manifest
    c_refact.localize_mutable_globals(path, compdb, root)
    assert "discarded" not in source.read_text()
    assert "unused_slot" not in source.read_text()
    assert "DiscardedType" not in source.read_text()
    assert "extern void harmless_header_declaration(Unused *);" in source.read_text()
    executable = tmp_path / "rewritten"
    hermetic.run(["clang", "-x", "c", source, "-o", executable], check=True, capture_output=True)
    assert hermetic.run([executable], check=False).returncode == 1


def test_contract_failure_never_retries_or_changes_disposition(tmp_path, source_pangs, monkeypatch):
    root = tmp_path / "project"
    code = "static int g; int main(void){return ++g;}"
    source, compdb, path = analyze(source_pangs, root, code)
    original_manifest = path.read_bytes()
    attempts = []

    def materialize(manifest, database, staged):
        attempts.append((staged / source.name).read_text())
        (staged / source.name).write_text("damaged first attempt")
        raise ValueError("injected unsupported representation")

    monkeypatch.setattr(c_refact, "_localize_mutable_globals_in_place", materialize)
    with pytest.raises(pangs_source.ContractViolation, match="contract violated"):
        c_refact.localize_mutable_globals(path, compdb, root)
    assert attempts == [code]
    assert source.read_text() == code
    assert path.read_bytes() == original_manifest


@pytest.mark.parametrize(
    "code,expected",
    [
        (
            "static float grad(int n){static float basis[2][2]={{1,2},{3,4}};"
            "float *p=basis[n&1]; return p[0]+p[1];}"
            "int main(int n,char **v){return (int)grad(n);}",
            {"grad.basis": "immutable"},
        ),
        (
            'static char *helperGood1(void){static char charString[]="abc";return charString;}'
            "int main(int n,char **v){return helperGood1()[n&1];}",
            {"helperGood1.charString": "immutable"},
        ),
        (
            "struct Node {int id; double value; int edges[2];};"
            "static struct Node node_storage[100];"
            "static void add_node(void){node_storage[0].id=42;}"
            "static void initialize_test_data(void){add_node();}"
            "int main(int n,char **v){return node_storage[n&1].id;}",
            {"node_storage": "immutable"},
        ),
        (
            'static char *default_colors[2]={"red","blue"};'
            'static char *default_colorblind_safe[2]={"black","white"};'
            "int main(int n,char **v){char **p=n&1?default_colors:default_colorblind_safe;"
            "return p[n&1][0];}",
            {"default_colors": "localize", "default_colorblind_safe": "localize"},
        ),
        (
            "static int g=7; int main(void){if(0) g=9;return g;}",
            {"g": "localize"},
        ),
        (
            "static int g=7; int main(void){int *p=&g;return *p;}",
            {"g": "immutable"},
        ),
        (
            "static int g=7,h=8; int main(void){if(0)g=9;return g+h;}",
            {"g": "unhandled", "h": "immutable"},
        ),
        (
            "static unsigned g=1u+2u; int main(void){return g;}",
            {"g": "localize"},
        ),
        (
            "static int callee(void){return 7;} static int (*g)(void)=callee;"
            "int main(void){return g();}",
            {"g": "immutable"},
        ),
        (FOLDED_SOURCE, {"g": "immutable"}),
        (
            FOLDED_SOURCE.replace("return _Generic", "if(0)g=9; return _Generic"),
            {"g": "localize"},
        ),
        (
            "static int g=7; int main(void){if(0)(void)sizeof(int[++g]);return g;}",
            {"g": "localize"},
        ),
    ],
    ids=[
        "basis",
        "charString",
        "node_storage",
        "palettes",
        "retained-write",
        "address-only",
        "joined-declaration",
        "section-initializer",
        "function-pointer-is-sync",
        "folded-source-immutable",
        "folded-source-localize",
        "sizeof-vla-retained-update",
    ],
)
def test_initial_disposition_and_actual_rust_materialization(
    tmp_path, source_pangs, code, expected
):
    root = tmp_path / "project"
    _, compdb, path = analyze(source_pangs, root, code, localize_only=False)
    initial = json.loads(path.read_text())
    globals_ = {g["meta"]["llvm_name"]: g for g in initial["globals"]}
    for name, strategy in expected.items():
        g = globals_[name]
        assert g["facts"]["written"]["value"] is False
        assert g["disposition"]["chosen"] == strategy, g
    guidance = root / "xj-guidance.json"
    translation_preparation.add_immutable_dispositions_to_guidance(path, guidance)
    c_refact.localize_mutable_globals(path, compdb, root)
    materialized = json.loads(path.read_text())
    assert materialized["globals"] == initial["globals"]
    assert not materialized.get("materialization", {}).get("demotions")
    rust = tmp_path / "rust"
    hermetic.run(
        [
            repo_root.find_repo_root_dir_Path()
            / "c2rust"
            / "target"
            / os.environ.get("XJ_BUILD_RS_PROFILE", "debug")
            / "c2rust",
            "transpile",
            root / "compile_commands.json",
            "-o",
            rust,
            "--emit-build-files",
            "--disable-refactoring",
            "--guidance",
            guidance,
        ],
        check=True,
        capture_output=True,
    )
    emitted = "\n".join(p.read_text() for p in rust.rglob("*.rs"))
    for name, strategy in expected.items():
        spelling = name.rsplit(".", 1)[-1]
        if strategy == "immutable":
            assert re.search(rf"\bstatic\s+{spelling}\s*:", emitted), emitted
        elif strategy == "localize":
            assert not re.search(rf"\bstatic\s+(?:mut\s+)?{spelling}\s*:", emitted), emitted
            assert "XjGlobals" in emitted
        else:
            assert re.search(rf"\bstatic\s+mut\s+{spelling}\s*:", emitted), emitted
    if "node_storage" in expected:
        assert "fn add_node" not in emitted
        assert "fn initialize_test_data" not in emitted
    if "discarded" in code:
        assert "fn discarded" not in emitted
    hermetic.run(
        ["cargo", "check", "--manifest-path", rust / "Cargo.toml"],
        cwd=rust,
        check=True,
        capture_output=True,
    )
