import os
import hashlib
import json
import subprocess
from pathlib import Path
from typing import Literal, TypedDict

type InterceptedBuildType = Literal["cc", "ld", "ar"]


class InterceptedCommandInfo(TypedDict):
    type: InterceptedBuildType
    directory: str
    arguments: list[str]
    file: str | None
    output: str | None


def is_executable_file(path: str) -> bool:
    return os.path.isfile(path) and os.access(path, os.X_OK)


def resolve_sans_intercept(name: str) -> Path:
    """Resolve the given command name to an absolute path, bypassing any intercept wrappers."""
    # Note a subtlety of the PATH setup here. Example:
    #  - The user runs `./cli/10j pytest tests -k triplicated`
    #  - 10j invokes pytest with PATH extended with Tenjin deps
    #  - pytest invokes do_translate() with buildcmd="make"
    #  - do_translate runs compute_build_info_in, which runs `make` with
    #    cli/sh/cc-ld-intercept as the first PATH entry.
    #  - `make` invokes cli/sh/cc-ld-intercept/cc
    #  - which runs $CLI_DIR/10j intercept-exec cc "$@"
    #  - So now we're here, with an extended PATH. But when *we* invoke cc,
    #    we don't want the intercept script. So we'll search the PATH ourselves,
    #    skipping our cc-ld-intercept directory.
    for pd in os.environ.get("PATH", "").split(os.pathsep):
        if "cc-ld-intercept" in pd:
            continue
        candidate = os.path.join(pd, name)
        if is_executable_file(candidate):
            return Path(candidate)
    raise FileNotFoundError(f"Could not find non-intercepted command for {name}")


# Integrated functionality from c2rust/scripts/cc-wrappers/common.py
# which does not require Python to be installed outside the hermetic environment.
def intercept_exec(build_type: InterceptedBuildType, run_as: Path, args: list[str]) -> int:
    # When CMake invokes us as a wrapper, it will pass an absolute path for the
    # real compiler as the first argument. When invoked directly, the first argument
    # will just be an argument like "-c" or a source file name.

    if len(args) > 0 and os.path.isabs(args[0]) and is_executable_file(args[0]):
        arguments = args
    else:
        non_intercepted_command = resolve_sans_intercept(run_as.name)
        arguments = [non_intercepted_command.as_posix(), *args]

    build_commands_dir = os.environ.get("BUILD_COMMANDS_DIRECTORY", "/tmp/build_commands")
    # Ensure the build commands directory exists (concurrency-safe)
    Path(build_commands_dir).mkdir(parents=True, exist_ok=True)
    build_info: InterceptedCommandInfo = {
        "type": build_type,
        "directory": os.getcwd(),
        "arguments": arguments,
        "file": None,
        "output": None,
    }
    build_json = json.dumps(build_info, indent=4)

    # Hash the contents of the JSON file and use that as the file name
    # This is safe for concurrency, and guarantees that each unique
    # compilation gets an output file
    hm = hashlib.sha256()
    hm.update(build_json.encode("utf-8"))
    build_file_name = "%s.json" % hm.hexdigest()
    build_file = os.path.join(build_commands_dir, build_file_name)
    with open(build_file, "w", encoding="utf-8") as f:
        f.write(build_json)

    script_dir = os.path.dirname(os.path.realpath(__file__))
    # The -B flag tells gcc/clang to look in the given directory
    # for compiler executables before looking in the system paths.
    # This ensures that when a build script uses "clang" to do linking,
    # clang will use our `ld.lld` wrapper script.
    if build_type == "cc":
        arguments.insert(1, "-B" + script_dir)

    return subprocess.call(arguments)
