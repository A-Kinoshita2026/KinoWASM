# Testing

English | [日本語](ja/Testing.md)

The repository contains project-owned regression sources and drivers. Official WebAssembly/WASI suites are acquired separately; neither their source checkouts nor generated inputs are bundled in the public repository.

## Prepare test inputs

Configure and build with Git and wabt available. `Test/CMakeLists.txt` pins the specification checkout revision and converts `.wast` inputs with `wast2json`. Sources are stored in `Materials/wasm-spec/`, and generated `.json` / `.wasm` files in `Bin/Test/testsuite/`. Project inputs under `Test/wasi/` are generated into the same test tree.

The separate upstream WASI suite is acquired under `Materials/wasi-testsuite/`. Its runner is used with `tools/wasi-testsuite-adapter/kinoruntime.py`; it is distinct from the project WASI regression inputs processed by `testsuite_runner`.

Missing tools or incomplete checkouts can leave inputs ungenerated. Confirm that generation completed before treating a runner result as full-suite coverage.

## Full-suite execution

```bat
cd Bin
testsuite_runner.exe
```

Run from `Bin/`. The runner reads JSON commands and reports `passed=N failed=M`, with `skipped=K` where applicable. Supported directives include module loading, registration, actions, expected returns/NaNs, traps, exhaustion, and malformed/invalid/unlinkable/uninstantiable module assertions.

The clean public-source build produced a total of **45,037 passed / 0 failed**, across 258 result reports, on 2026-09-30. Counts are a snapshot of the pinned inputs, project regressions and feature settings, not a universal conformance claim.

The older workspace total of 45,500 included seven leftover JSON inputs that the current CMake configuration does not generate: five under `proposals/exception-handling/`, plus `proposals/wasm-3.0/binary.json` and `if.json`. Their 463 successful assertions account for the difference. Use a clean generated tree when comparing coverage.

Any failed assertion requires investigation. A missing `passed=` result for an individual input is a crash. Check the process exit code as well as totals; never count missing cases as success.

Separate stderr from stdout when aggregating counts, because runtime diagnostics can interrupt result lines. For example, in a Windows command prompt:

```bat
testsuite_runner.exe > testsuite-results.txt 2> testsuite-diagnostics.txt
```

These files are local outputs under the ignored `Bin/` directory.

## Individual inputs

```bat
testsuite_runner.exe Test\testsuite\nop.json
testsuite_runner.exe Test\testsuite\proposals\wasm-3.0
```

Use an individual input to investigate crashes or failed assertions, then rerun the full suite after fixing the cause.

## Feature settings and binary identity

SIMD execution is not implemented in the core engine. SIMD is disabled by default, so associated inputs are skipped. Enabling `KINOWASM_ENABLE_SIMD` can cause those cases to run and fail; do not compare their totals against the default configuration as if the binaries were identical.

All configurations copy executables into the same `Bin/` directory. Verify the binary configuration before every measurement. After an alternate build, restore the intended executables explicitly or rebuild the corresponding targets; a no-op incremental build does not rerun post-build copies.

## Project regression checks

| Area | Inputs/drivers |
|---|---|
| WASI arguments, time, filesystem, VFS, write/link operations and invalid addresses | `Test/wasi/wasi_*.wast` |
| Validation, exception handling, references, tables and value propagation | `Test/wasi/core_*.wast` |
| Suspend/resume and exceptions across resume | `kw_core_yieldtest` |
| Store lifetime and failed-build cleanup | `kw_core_lifetest` |
| Host callback reentry | `kw_core_reentrytest` |
| Dynamic push/invoke/pop lifecycle | `KinoRuntimePushPopTest` |
| Allocator/container behavior | `kutil_microtest` |

Examples, with `Bin/` as the working directory:

```bat
kw_core_yieldtest.exe
kw_core_lifetest.exe
kw_core_reentrytest.exe
kutil_microtest.exe arraytest1
```

`KinoRuntimePushPopTest.exe` remains in the build directory. Check `Test/core/CMakeLists.txt` and `Test/util/CMakeLists.txt` for the current CTest registration and required working directories. The full testsuite command above remains the explicit full-suite validation route.

CTest also runs the full specification/project suite and the yield, lifetime and reentry drivers when their generated inputs are available:

```bat
ctest --test-dir out/build/x64-Release --output-on-failure
```

Run this command from the repository root. The testsuite entry uses `Bin/` as its working directory and rejects reported failed assertions. Validate the suite's generated inputs and result coverage as described above.

Reset global registration/allocation state between independent tests. Preserve suspended execution frames until the corresponding operation completes; resolve test isolation through reset/cleanup rather than premature frame destruction.

## Adding tests

Add project `.wast` regressions under `Test/wasi/`, register their generation in `Test/CMakeLists.txt`, and assert both the expected value and failure behavior where relevant. Standalone drivers belong under `Test/core/`; utility checks belong under `Test/util/`.

CoreMark is an external workload and must be supplied separately. `KinoRuntimePerfInvokeFunc2` measures a sample `fibonacci` export, while `KinoRuntimePerfLoad` measures loading. These are different measurements and depend on the selected module.

Related: [Build and test](Build-and-Test.md), [Error handling](Error-Handling.md), [Host functions](Host-Functions.md).
