# Build and test

English | [日本語](ja/Build-and-Test.md)

Run configuration and build commands from the repository root. Runtime sources are in `KinoWASM/`, the CLI entry point is `apps/KinoRuntime/main.c`, and shared host code is in `host/`.

## Requirements

- Windows x64 and an AVX2-capable CPU for the current top-level build.
- CMake 4.2 or later and Ninja.
- MSVC 14.50 or later, or clang-cl, with guaranteed tail-call support. Unsupported compilers are rejected during configuration.
- Git for acquiring upstream test repositories.
- wabt (`wast2json`, `wat2wasm`) on `PATH` for generating tests. A missing tool can leave test inputs ungenerated even if the C targets build successfully.

Use a Visual Studio developer command prompt with the intended compiler enabled. The clang presets resolve `clang-cl` from `PATH`; enable the intended LLVM toolchain before configuring.

## Presets

| Preset | Compiler | Configuration |
|---|---|---|
| `x64-Debug` | MSVC | Debug |
| `x64-Release` | MSVC | Release |
| `x64-clang-Debug` | clang-cl | Debug |
| `x64-clang-Release` | clang-cl | Release |

PGO presets are optional development configurations; ordinary builds do not require them.

```bat
cmake --preset x64-Release
cmake --build out/build/x64-Release -j 1
```

For Debug, use `x64-Debug` in both commands. MSVC Debug uses `/O2 /Ob2` to satisfy `musttail` requirements and disables conflicting `/RTC1` runtime checks. Debug information and the Debug CRT are retained, but optimized variables and stepping may not match source statements exactly.

Build products are under `out/build/<preset>/`. Most executables are copied to the shared `Bin/` directory. Building another configuration can overwrite them. An incremental build with “no work to do” does not restore executables overwritten by another configuration: explicitly copy the intended build products or rebuild the targets.

## Targets

| Target | Purpose |
|---|---|
| `KinoWASM` | Runtime static library, under the build directory's `KinoWASM/` subdirectory |
| `KinoUtil` | Allocation and container support library |
| `KinoRuntime` | CLI for WASM execution |
| `testsuite_runner` | Official specification tests and project regression inputs |
| `KinoRuntimePerfInvokeFunc2` | Repeated calls to a sample `fibonacci` export |
| `KinoRuntimePerfLoad` | Module loading measurements |
| `KinoRuntimePushPopTest` | Dynamic module lifecycle regression; remains in the build directory |
| `kw_core_run`, `kw_core_apitest`, `kw_core_multitest` | Standalone execution and API drivers |
| `kw_core_yieldtest`, `kw_core_lifetest`, `kw_core_reentrytest` | Suspension, store lifetime and host reentry regressions |
| `kutil_microtest` | Allocation/container checks and measurements |

## Run a module

```bat
Bin\KinoRuntime.exe path\to\module.wasm [args...]
```

This is the WASI command-line route. The application also has a legacy mode for local `Library.wasm` / `Start.wasm` inputs; those files are not prepared automatically by the C build.

## Run the full testsuite

CMake acquires the specification suite in `Materials/wasm-spec/` and generates `.json` / `.wasm` files under `Bin/Test/testsuite/`. Project inputs in `Test/wasi/` are also converted. Upstream sources and generated inputs are excluded from Git.

```bat
cd Bin
testsuite_runner.exe
```

See [Testing](Testing.md) for individual cases, skipped features and result aggregation.

## Troubleshooting

| Symptom | Check |
|---|---|
| Compiler rejected | Use a supported MSVC or clang-cl version |
| `wast2json` or `wat2wasm` missing | Install wabt, add it to `PATH`, and configure again |
| Upstream clone fails | Check Git/network access or prepare the checkout at the revision specified in `Test/CMakeLists.txt` |
| Test directory not found | Run from `Bin/` and verify that generation completed |
| Copy failures during a build | Use `-j 1`; avoid concurrent builds writing to `Bin/` |
| Stale objects after a structure/header change | Use a new build directory or `--clean-first` |

Related: [API](Public-API.md), [Testing](Testing.md), [Error handling](Error-Handling.md).
