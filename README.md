# KinoRuntime / KinoWASM

English | [日本語](README.ja.md)

**KinoWASM** is a WebAssembly runtime written in C for embedding in host applications. **KinoRuntime** is the accompanying command-line application, with test runners and measurement tools.

The execution engine uses a register-TOS, direct-threaded architecture with guaranteed tail calls (`musttail`).

## Console platforms and production use

KinoWASM has been built for Nintendo Switch, Nintendo Switch 2, PlayStation 5 and Xbox, and is used in several production titles.

Console integrations are maintained privately and require the respective platform SDKs and host implementations. This public repository provides the Windows x64 build; console SDKs, platform-specific integrations and console build instructions are not included.

## WebAssembly feature support

✅ Implemented · 🟡 Partial support or validation limitations · ❌ Not supported by the execution engine. This table describes the default core engine, not certification of complete specification conformance.

| Feature / instruction family | Status | Scope |
|---|:---:|---|
| Core scalar instructions | ✅ | `i32`, `i64`, `f32`, `f64`: arithmetic, comparisons, conversions and reinterpretation |
| Control flow and calls | ✅ | Blocks, loops, branches, direct/indirect calls, locals and globals |
| Linear memory | ✅ | Loads/stores, `memory.size`, `memory.grow` |
| Mutable globals | ✅ | Imported/exported mutable globals |
| Sign-extension operators | ✅ | `i32.extend8_s` / `extend16_s`, `i64.extend8_s` / `extend16_s` / `extend32_s` |
| Non-trapping float-to-int conversions | ✅ | `i32.trunc_sat_*`, `i64.trunc_sat_*` |
| Multi-value | ✅ | Multiple function results and block/loop parameters |
| Bulk memory and table operations | ✅ | Memory/table init, copy and fill; data/element drop; table grow and size |
| Reference types | ✅ | Basic `funcref` / `externref`, `ref.null`, `ref.is_null`, `ref.func` and table operations |
| Tail calls | ✅ | `return_call`, `return_call_indirect` |
| Extended constant expressions | ✅ | Integer `add`, `sub`, `mul` in constant expressions |
| Multiple memories | ✅ | Indexed memory operations |
| Memory64 / Table64 | 🟡 | Implemented paths and selected tests; some boundary validation cases remain unsupported |
| Exception handling | 🟡 | Legacy `try`/`catch` and newer `try_table` / `throw_ref` implemented; upstream coverage is limited by test-tool syntax support |
| Wide arithmetic | ✅ | `i64.add128`, `i64.sub128`, `i64.mul_wide_s`, `i64.mul_wide_u` |
| SIMD / Relaxed SIMD | ❌ | No core execution support; enabling the SIMD parser option does not enable execution |
| Threads / atomics | ❌ | Atomic instructions are rejected; no WebAssembly threading support |
| GC / typed function references | ❌ | GC types/operations and `call_ref` / `return_call_ref` are not supported |

WASI is a separate host interface and is partially supported; see [Host functions](docs/Host-Functions.md). See [Testing](docs/Testing.md) for coverage and skipped cases. Support for selected newer features does not imply full WebAssembly 3.0 support.

## Getting started

- [Embedding guide](docs/QuickGuide.md)
- [Build and run](docs/Build-and-Test.md)
- [Public C API](docs/Public-API.md)
- [Host functions and WASI](docs/Host-Functions.md)
- [Documentation index](docs/README.md)

## Repository layout

| Directory | Purpose |
|---|---|
| `KinoWASM/` | The runtime library; start with `kinowasm.h` |
| `apps/KinoRuntime/` | The CLI application |
| `host/` | Shared host implementations: WASI, Windows memory backend, allocation and diagnostics |
| `Test/` | Project tests, upstream test runners and measurement harnesses |
| `WASMData/` | Project-owned WASM samples and build scripts |
| `tools/`, `scripts/` | Development and build utilities |
| `docs/` | English documentation; Japanese editions are in `docs/ja/` |

The code in `host/` is a reference for the application side of the integration. Adapt it to your platform, memory management and permission requirements.

## Build and run

The top-level build currently targets Windows x64. It requires CMake 4.2 or later, Ninja, and a compiler supporting guaranteed tail calls: MSVC 14.50 or later, or clang-cl. The current build enables AVX2. Generating test inputs also requires Git and wabt (`wast2json`, `wat2wasm`).

From a compatible Visual Studio developer command prompt, run these commands at the repository root:

```bat
cmake --preset x64-Release
cmake --build out/build/x64-Release -j 1
Bin\KinoRuntime.exe path\to\module.wasm
```

Run the complete testsuite with `Bin/` as the working directory:

```bat
cd Bin
testsuite_runner.exe
```

Build configurations share `Bin/`. Make sure its executables belong to the configuration you intend to run. See [Testing](docs/Testing.md) for input generation and result interpretation.

## Scope and limitations

- WASI support is partial; see [Host functions](docs/Host-Functions.md). Full WASI or POSIX compatibility is not claimed.
- SIMD is disabled by default. The option exists, but SIMD execution is not implemented in the core engine.
- Embedding requires a host-provided linear-memory backend and caller-owned store/module memory.
- This project does not claim a completed security audit or guaranteed safety for untrusted modules. Host functions determine the capabilities exposed to a module.
- CoreMark and upstream testsuite contents are not bundled in the public source distribution.

## License

Licensed under the [MIT License](LICENSE). Copyright (c) 2026 A.Kinoshita.

Separately acquired third-party test suites and toolchain libraries retain their own licenses.
