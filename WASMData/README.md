# WASM samples

English | [日本語](README.ja.md)

This directory contains project-owned sample sources and build scripts. It does not contain CoreMark or the official testsuite.

| Binary | Corresponding source/content | Distribution |
|---|---|---|
| `library.wasm` | `Library.c`: `ConsoleTest`, `SubTest`, `fibonacci`; imports `env.Test1` and `env.Console` | Retained as a project sample |
| `start.wasm` | `System/Main.cpp`, `Adv.cpp`: exports `Main`, imports adventure host functions | Retained as a project sample |
| `fibtest.wasm` | `fibtest.c`: Fibonacci checking and output, with linked toolchain libraries | Generated locally and excluded from Git; source and build script retained |

This classification is based on imports, exports, strings and build options, not a byte-for-byte reproduction with the original compiler. The library/adventure scripts use `-nostdlib`.

## Regenerate

Enable Emscripten's `emcc` / `em++` on `PATH`, then run these commands with this directory as the working directory:

```bat
build_fibtest.bat
build_adv.bat
build_library.bat
```

- `build_fibtest.bat` generates the ignored `fibtest.wasm` here.
- `build_adv.bat` generates `start.wasm` here.
- `build_library.bat` generates `library.wasm` here.

Redistributing regenerated binaries that contain toolchain libraries requires checking those libraries' license conditions. The public source distribution excludes the generated Fibonacci binary.

Supply CoreMark separately. Official testsuite sources and generated inputs are acquired under the ignored `Materials/` and `Bin/` directories; see [Testing](../docs/Testing.md).
