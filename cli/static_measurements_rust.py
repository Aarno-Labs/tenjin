import json

from pathlib import Path
import subprocess

import hermetic


def static_rust_metrics(cargo_project_dir: Path) -> dict[str, int | float]:
    """Compute static measurements for a Rust project."""
    assert cargo_project_dir.is_dir(), "The provided path must be a directory"

    caveman_metrics = compute_caveman_safety_metrics(cargo_project_dir)
    lints_count = count_rustc_and_clippy_lints(cargo_project_dir)

    return {
        **caveman_metrics,
        **lints_count,
    }


def compute_caveman_safety_metrics(cargo_project_dir: Path) -> dict[str, int | float]:
    files_count = 0
    file_lines_count = 0
    total_fns_count = 0
    total_unsafe_fns_count = 0

    # Eventually we can do fancy syn-based parsing to avoid false positives
    # from string literals, but for now we do the caveman thing.
    def process_file(file_path: Path):
        nonlocal files_count, file_lines_count, total_fns_count, total_unsafe_fns_count

        with file_path.open() as f:
            if file_path == cargo_project_dir / "build.rs":
                # Skip the build script, as it is not part of the main codebase.
                return
            files_count += 1
            for line in f:
                line = line.rstrip()
                if not line:
                    continue
                file_lines_count += 1
                if "fn " in line and "(" in line:
                    if line.endswith(";"):
                        # Declarations of functions without bodies don't count,
                        # for our purposes. If they're called, the unsafety will
                        # be accounted for elsewhere.
                        continue
                    total_fns_count += 1
                    # print(f"Found function in {file_path}:", line)
                    if "unsafe " in line:
                        total_unsafe_fns_count += 1

    for file_path in cargo_project_dir.glob("**/*.rs"):
        if file_path.is_file():
            process_file(file_path)

    if total_fns_count == 0:
        print("Warning: No functions found in the codebase. Cannot compute unsafe function ratio.")
        print("Inspected files in cargo project directory:", cargo_project_dir)
        for file_path in cargo_project_dir.glob("**/*.rs"):
            if file_path.is_file():
                print(" -", file_path)
        raise ValueError("No functions found in the codebase")
    total_unsafe_fns_ratio = total_unsafe_fns_count / total_fns_count
    total_unsafe_fns_ratio = round(total_unsafe_fns_ratio, 3)

    return {
        "total_fns_count": total_fns_count,
        "total_unsafe_fns_count": total_unsafe_fns_count,
        "total_unsafe_fns_ratio": total_unsafe_fns_ratio,
        "files_count": files_count,
        "nonempty_lines_count": file_lines_count,
    }


def count_rustc_and_clippy_lints(cargo_project_dir: Path) -> dict[str, int]:
    rustc_errors = 0
    rustc_warnings = 0
    clippy_lints = 0

    messages, res = get_clippy_messages_json(cargo_project_dir)
    if res.returncode != 0:
        rustc_errors += 1
        print("Error running clippy:\n", res.stderr)

    for obj in messages:
        message = obj["message"]
        mb_code: dict | None = message["code"]
        level = message["level"]

        if mb_code:
            kind: str = mb_code["code"]
            if kind.startswith("clippy::"):
                clippy_lints += 1
            else:
                match level:
                    case "error":
                        rustc_errors += 1
                        print(f"Rustc error found for {str(cargo_project_dir)}:\n", message)
                    case "warning":
                        rustc_warnings += 1
                    case _:
                        pass

    return {
        "rustc_errors": rustc_errors,
        "rustc_warnings": rustc_warnings,
        "clippy_lints": clippy_lints,
    }


def get_clippy_messages_json(
    cargo_project_dir: Path,
) -> tuple[list[dict], subprocess.CompletedProcess]:
    """Get Clippy messages for the given project in JSON format,
    and the `subprocess.CompletedProcess` object."""
    assert cargo_project_dir.is_dir(), "The provided path must be a directory"

    res = hermetic.run_cargo_on_translated_code(
        [
            hermetic.tenjin_multitool_toolchain_specifier(),
            "clippy",
            "--message-format",
            "json",
            "--manifest-path",
            (cargo_project_dir / "Cargo.toml").absolute().as_posix(),
            "--",
            "-Aclippy::missing_safety_doc",  # TRACTOR does not involve code comments.
            "-Aclippy::too_many_arguments",  # Robust automated conversion > clippy's ideas about style
            # Given C code with constant branches, it's not unreasonable to translate them as-is.
            "-Aclippy::absurd_extreme_comparisons",
        ],
        cwd=cargo_project_dir,
        text=True,
        capture_output=True,
        env_ext={"RUSTFLAGS": ""},  # avoid -Dwarnings from CI
        check=False,
    )

    messages = []
    for line in res.stdout.split("\n"):
        if line == "" or line[0] != "{":
            continue
        obj: dict = json.loads(line)
        if obj["reason"] != "compiler-message":
            continue
        messages.append(obj)

    return messages, res
