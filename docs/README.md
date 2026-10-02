# KinoWASM documentation

English | [日本語](ja/README.md)

Start with the [repository README](../README.md), then follow the embedding or CLI build instructions.

| Document | Contents |
|---|---|
| [QuickGuide](QuickGuide.md) | Initialize a runtime, provide memory, load and call modules, and integrate the library |
| [Public API](Public-API.md) | Public types, functions, ownership and call contracts |
| [Host functions](Host-Functions.md) | Imports, callbacks, WASI, suspension and module lifecycle |
| [Error handling](Error-Handling.md) | Results, traps and failure handling |
| [Build and test](Build-and-Test.md) | Toolchain requirements, presets, targets and CLI execution |
| [Testing](Testing.md) | Test inputs, runners, regression checks and result interpretation |
| [Cooperative example](../examples/cooperative/README.md) | Host suspension/resumption, main-thread scheduling and Unity/Unreal templates |
| [Benchmarks](benchmark.md) | Preliminary CLI runtime comparison using CoreMark and recursive Fibonacci |
| [Publishing](Publishing.md) | Create a reviewed source copy for a separate public repository |

English is the default documentation language. Japanese editions are available in `ja/`; each document links to its counterpart. Internal optimization records and historical design documents are outside the public documentation.

Where documentation differs from the implementation, check the current headers and source. The English guides have been organized around the public interface rather than reproducing every historical implementation note in the Japanese editions.
