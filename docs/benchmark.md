# Runtime benchmark snapshot

English | [日本語](ja/benchmark.md)

These are user-supplied measurements of the tested CLI binaries. CoreMark was run three times with approximately the same scores, according to the user. The tables show the supplied logs, not averages or medians. No additional measurements were performed for this document.

## Reported environment

| Item | User-reported configuration |
|---|---|
| CPU | Intel Core i7-13700F |
| OS | Windows 11 26H2 |
| iwasm | Fast interpreter, not JIT |
| KinoRuntime | Standard Clang build; compiler version and preset not recorded |
| wasm3 | Version and build configuration not recorded |

This environment information was supplied by the person who ran the benchmarks; it was not independently inspected.

## CoreMark

The score is the workload's reported **Iterations/Sec**; higher is better. Raw elapsed times are not comparable here because iwasm ran 30,000 iterations and the other runtimes ran 60,000.

| Runtime[^runtime] | Iterations/Sec | Iterations | Reported time (s) | Validation |
|---|---:|---:|---:|---|
| iwasm (WAMR) | 1734.417344 | 30000 | 17.296875 | Passed |
| wasm3 | 3555.555556 | 60000 | 16.875000 | Passed |
| KinoRuntime | 4385.746651 | 60000 | 13.680681 | Passed |

All three logs report successful operation validation. The runs exceed CoreMark's minimum ten-second duration, but the available metadata is insufficient to claim a fully documented or certified CoreMark result. See the [upstream run and reporting rules](https://github.com/eembc/coremark/blob/main/README.md).

Common workload metadata reported in the logs:

- CoreMark size: `666`; memory location: `STATIC`.
- WASM workload compiler: Clang 11.0.0, LLVM revision `176249bd6732a8044d457092ed932768724a6f06`; flags: `-O3`. These describe the guest module, not the compilers used to build the runtime executables.
- `seedcrc=0xe9f5`, `crclist=0xe714`, `crcmatrix=0x1fd7`, `crcstate=0x8e3a`.
- Final CRC: iwasm `0x5275`; wasm3/KinoRuntime `0xbd59`. Iteration counts also differ; each run's own validation result is recorded above.

In this snapshot, KinoRuntime reports approximately **1.23x** wasm3's throughput and **2.53x** iwasm's throughput. These ratios apply only to these reported runs.

## Recursive Fibonacci

The guest workload computes `fibonacci(40)`. Lower reported time is better.

| Runtime[^runtime] | Reported time (ms) | Result | Validation |
|---|---:|---:|---|
| iwasm (WAMR) | 4298.94 | 102334155 | `OK` |
| wasm3 | 2601.12 | 102334155 | `OK` |
| KinoRuntime | 1838.94 | 102334155 | `OK` |

The workload measures its calculation interval inside WASM. This is not process wall-clock time and does not include the complete module loading/startup path. The sample uses `clock_gettime(CLOCK_MONOTONIC)`, so the runtime's WASI clock implementation is part of the measurement path. See [sample sources](../WASMData/README.md).

The inverse-time ratios are approximately **1.41x** versus wasm3 and **2.34x** versus iwasm for this particular workload and these runs.

## Commands and scope

The supplied log used the same workload filename for all runtimes, from a Windows command prompt in `Bin/`:

```bat
iwasm coremark.wasm
wasm3 coremark.wasm
kinoruntime coremark.wasm
iwasm fibtest.wasm
wasm3 fibtest.wasm
kinoruntime fibtest.wasm
```

The guest compiler metadata matches across the CoreMark logs. Workload file hashes were not supplied, so identical module bytes have not been independently verified. Workload binaries are not included in this benchmark document or in the public source copy; CoreMark must be obtained separately and Fibonacci can be built from the project sample.

[^runtime]: These are comparisons through the respective CLI runtimes, including their selected execution backend, host functions, WASI timing behavior and build settings. They do not isolate dispatch-loop performance. iwasm is the CLI for [WAMR](https://github.com/wasm-micro-runtime/wasm-micro-runtime); the user identified this run as fast interpreter mode, not JIT. [wasm3](https://github.com/wasm3/wasm3) describes itself as a WebAssembly interpreter; the tested version/build configuration was not recorded.

## Metadata needed for a reproducible comparison

The CPU, OS and iwasm execution mode are recorded above. Still missing are the measurement date, runtime versions/commits, runtime compiler versions/flags, and KinoRuntime's preset/PGO/ADVENTURE settings. Binary/workload hashes and warmup details were not supplied. CoreMark was run three times with approximately the same scores, but the individual scores and statistical spread were not supplied; the Fibonacci repeat count is unrecorded. “Standard Clang build” does not establish the runtime build settings. The results remain a preliminary snapshot.

For a reproducible update, record those details, use identical workload bytes, run several rounds in alternating runtime order, and report the median and spread. Keep guest compilation settings separate from runtime build settings. Rebuild/restore the intended configuration before using the shared `Bin/` directory. One CoreMark workload and one recursive Fibonacci workload do not establish overall application performance.
