# KinoRuntime CLI

English | [日本語](README.ja.md)

This application embeds KinoWASM to execute WASM modules.

- `main.c`: command-line handling and execution entry point.
- `adventure.c`: host functionality used when `KINOWASM_ENABLE_ADVENTURE` is enabled.

Shared host code is in `host/`, and the runtime library is in `KinoWASM/`. Build from the repository root:

```bat
cmake --preset x64-Release
cmake --build out/build/x64-Release -j 1
Bin\KinoRuntime.exe path\to\module.wasm
```

See [Build and test](../../docs/Build-and-Test.md).
