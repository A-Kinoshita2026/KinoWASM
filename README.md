# KinoRuntime / KinoWASM

English | [日本語](README.ja.md)

**KinoWASM** is a WebAssembly runtime written in C for embedding in host applications. **KinoRuntime** is the accompanying command-line application, with test runners and measurement tools.

The execution engine uses a register-TOS, direct-threaded architecture with guaranteed tail calls (`musttail`).

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
