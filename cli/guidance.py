import json
from pathlib import Path

from constants import XJ_GUIDANCE_FILENAME


def store_in_codebase(guidance: dict, codebase: Path, name: str = XJ_GUIDANCE_FILENAME):
    """Serialize guidance as json to codebase / name"""
    json.dump(
        guidance,
        open(codebase / name, "w", encoding="utf-8"),
        indent=2,
    )


def load_from_codebase(codebase: Path, name: str = XJ_GUIDANCE_FILENAME) -> dict:
    """Deserialize json guidance as dict from codebase / name"""
    return load_and_parse_guidance(codebase / name)


def load_and_parse_guidance(guidance_path_or_literal: Path | str) -> dict:
    if isinstance(guidance_path_or_literal, Path):
        return json.load(guidance_path_or_literal.open("r", encoding="utf-8"))
    elif type(guidance_path_or_literal) is str:
        try:
            if guidance_path_or_literal == "":
                guidance = {}
            else:
                guidance = json.loads(guidance_path_or_literal)
        except json.JSONDecodeError:
            guidance = json.load(Path(guidance_path_or_literal).open("r", encoding="utf-8"))
        return guidance
    else:
        assert False


def parse_decl_specifier(s: str) -> tuple[str, str | None, str | None, str | None]:
    """Parse <function name>:<var name>@<line number>#<file path>"""

    def split_from_back(inp: str, at: str) -> tuple[str, str | None]:
        res = inp.split(at)
        assert len(res) <= 2
        if len(res) == 1:
            return (res[0], None)
        else:
            assert len(res) == 2
            return (res[0], res[1])

    s, prefix = split_from_back(s, "@")
    s, linum = split_from_back(s, "#")
    fn, v = split_from_back(s, ":")
    return (fn, v, linum, prefix)


def make_decl_specifier_str(fn: str, v: str | None, linum: str | None, filename: str | None) -> str:
    """Construct string representation of declspec pieces (suitable for serializing to JSON)"""

    def prefix(p: str, x: str | None):
        if x is None:
            return ""
        else:
            return f"{p}{x}"

    return fn + prefix(":", v) + prefix("#", linum) + prefix("@", filename)


def map_guidance_value(guidance: dict, key: str, f):
    """Assuming guidance[key] is a dict, map f on the kv pairs of guidance[key].
    f should return a new kv pair"""
    if key not in guidance:
        return

    obj = guidance[key]
    new_obj = {}
    for k, v in list(obj.items()):
        k2, v2 = f(k, v)
        new_obj[k2] = v2
    guidance[key] = new_obj


def map_function_names(guidance: dict, f):
    """Transform the function names in guidance (in place) by applying f to them"""

    def ap_specifier(sp: str) -> str:
        (fn, v, line, filename) = parse_decl_specifier(sp)
        fn2 = f(fn)
        return make_decl_specifier_str(fn2, v, line, filename)

    def apply_vars_of_type(ty: str, spec: str | list[str]):
        if type(spec) is str:
            return (ty, ap_specifier(spec))
        else:
            return (ty, [ap_specifier(s) for s in spec])

    def apply_vars_mut(sp: str, b: str):
        return (ap_specifier(sp), b)

    def apply_fn_return_type(fn: str, t: str):
        return (f(fn), t)

    map_guidance_value(guidance, "vars_of_type", apply_vars_of_type)
    # TODO 'declspecs_of_type' appears to be unimplemented in rust,
    # format with fn prefix may not be correct
    map_guidance_value(guidance, "vars_mut", apply_vars_mut)
    map_guidance_value(guidance, "fn_return_type", apply_fn_return_type)
