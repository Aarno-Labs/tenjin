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
                "version": 2,
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

    def fail(_manifest, _compdb, _prev, staged):
        (staged / "test.i").write_text("broken")
        raise ValueError("intentional materializer failure")

    monkeypatch.setattr(c_refact, "_localize_mutable_globals_in_place", fail)
    with pytest.raises(ValueError, match="intentional"):
        c_refact.localize_mutable_globals(manifest_path, compdb, root, root)
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
    root.mkdir()
    source = root / "test.nolines.i"
    source.write_text(code)
    (root / "xj-guidance.json").write_text("{}")
    compdb = compilation_database.synthetic_compile_commands_for_c_file(source, root)
    compdb.to_json_file(root / "compile_commands.json")
    bc = root / "linked_module.bc"
    database = root / "effective.json"
    llvm_bitcode_linking.compile_and_link_bitcode(
        compdb, bc, use_llvm14=True, source_compdb_path=database
    )
    overrides = root / "overrides.toml"
    overrides.write_text('[cascade]\norder = ["localize"]\n' if localize_only else "")
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
    return source, compdb, root / "pangs-disposition" / "pangs-manifest.json"


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
    c_refact.localize_mutable_globals(manifest_path, compdb, root, root)
    rewritten = source.read_text()
    assert "Callback__pangs_context" in rewritten
    assert "dead(struct XjGlobals *xjg" in rewritten
    assert "p->fn(xjg)" in rewritten
    assert "ordinary(struct XjGlobals *xjg" in rewritten
    assert c_refact.XJG_PLACEHOLDER not in rewritten
    executable = tmp_path / "after"
    hermetic.run(["clang", "-x", "c", source, "-o", executable], check=True, capture_output=True)
    after = [hermetic.run([executable, *args], check=False).returncode for args in ([], ["arg"])]
    assert before == after == [3, 3]
    materialized = json.loads(manifest_path.read_text())
    assert materialized["materialization"]["c_validation"] == ["clang-14", "clang-21"]
    assert materialized["context_rewrite"]["fields"] == manifest["context_rewrite"]["fields"]


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
    c_refact.localize_mutable_globals(path, compdb, root, root)
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

    def materialize(manifest, database, prev, staged):
        attempts.append((staged / source.name).read_text())
        (staged / source.name).write_text("damaged first attempt")
        raise ValueError("injected unsupported representation")

    monkeypatch.setattr(c_refact, "_localize_mutable_globals_in_place", materialize)
    with pytest.raises(pangs_source.ContractViolation, match="contract violated"):
        c_refact.localize_mutable_globals(path, compdb, root, root)
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
    c_refact.localize_mutable_globals(path, compdb, root, root)
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
