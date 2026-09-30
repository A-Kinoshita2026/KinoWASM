# Reference host implementations

English | [日本語](README.ja.md)

The runtime library is in `../KinoWASM/`. This directory contains host code shared by the CLI and tests. Adapt or replace it for your platform, permissions and memory management.

| Files | Purpose |
|---|---|
| `extrafunction.c` | Host functions, including WASI |
| `kw_core_mem_backend_win.h` | Windows linear-memory backend |
| `winapi.c`, `winapi.h` | Windows wrappers |
| `systemmemory.c`, `systemmemory.h` | Host allocation arena setup |
| `debug.c`, `debug.h` | Timing and diagnostic output |
| `debug_opcode_names.inc` | Generated opcode names; regenerate with `tools/gen_opcode_names.py` |

See [Embedding](../docs/QuickGuide.md) and [Host functions](../docs/Host-Functions.md).
