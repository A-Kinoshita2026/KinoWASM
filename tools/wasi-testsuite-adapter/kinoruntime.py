"""KinoRuntime adapter for the WebAssembly/wasi-testsuite Python test runner.

Drop this file into the `adapters/` lookup path (or pass it via
`./run-tests --runtime <path-to-this-file>`) when running wasi-testsuite.

The runtime path defaults to `KinoRuntime.exe` (looked up via PATH). Override
with the `KINORUNTIME` environment variable, e.g.

    KINORUNTIME=D:/Projects/KinoRuntime/Bin/KinoRuntime.exe \
        python -m wasi_test_runner --runtime adapters/kinoruntime.py ...

The CLI surface implemented by `KinoRuntime.exe` is intentionally aligned with
wasmtime's: `--env KEY=VAL`, `--dir HOST::GUEST`, then the wasm file path,
followed by user args.
"""
import os
import shlex
import subprocess
from pathlib import Path
from typing import Dict, List, Tuple


# Allow `KINORUNTIME="C:/path/KinoRuntime.exe --some-flag"` for prepending
# extra runtime flags. On Windows, posix=False keeps backslashes in paths
# from being eaten by shlex (`D:\...` would otherwise vanish).
_IS_WINDOWS = os.name == "nt"
KINORUNTIME = shlex.split(
    os.getenv("KINORUNTIME", "KinoRuntime.exe"),
    posix=not _IS_WINDOWS,
)


def get_name() -> str:
    return "KinoRuntime"


def get_version() -> str:
    # `KinoRuntime.exe --version` prints e.g. "kinoruntime 0.1.0".
    result = subprocess.run(
        KINORUNTIME[0:1] + ["--version"],
        encoding="UTF-8",
        capture_output=True,
        check=True,
    )
    first_line = result.stdout.splitlines()[0].strip()
    parts = first_line.split(" ", 1)
    return parts[1] if len(parts) > 1 else first_line


def get_wasi_versions() -> List[str]:
    # KinoRuntime currently implements wasi_snapshot_preview1 only.
    return ["wasm32-wasip1"]


def get_wasi_worlds() -> List[str]:
    return ["wasi:cli/command"]


def compute_argv(
    test_path: str,
    args_env_dirs: Tuple[List[str], Dict[str, str], List[Tuple[Path, str]]],
    proposals: List[str],
    wasi_world: str,
    wasi_version: str,
) -> List[str]:
    argv: List[str] = []
    argv += KINORUNTIME
    args, env, dirs = args_env_dirs

    for k, v in env.items():
        argv += ["--env", f"{k}={v}"]

    for host, guest in dirs:
        argv += ["--dir", f"{host}::{guest}"]

    # Use `--` so that wasm files starting with `-` (unlikely, but possible)
    # are not mistaken for runtime flags.
    argv += ["--", test_path]
    argv += args

    return argv
