# Cooperative execution on the main thread

English | [日本語](README.ja.md)

KinoWASM lets a host import suspend a guest invocation and return control to the application. Later, `kinowasm_resume` continues immediately after that imported call, preserving the guest's call stack, locals and operands. A script can wait for the next frame, a dialogue choice or completion of an engine operation without blocking in the host callback.

The guest does not need to turn its entire control flow into a frame-by-frame state machine. The host decides when to resume. For game engines, invoke/resume on the main thread so host callbacks can use main-thread-only engine APIs. The example uses no worker thread and no guest-code transformation such as Asyncify.

## Execution contract

1. The guest calls `env.next_frame()` (no parameters or result).
2. The callback returns the application-defined signal `0x2000`, rather than `RES_SUCCESS`.
3. `kinowasm_invoke` or `kinowasm_resume` returns that signal to the host.
4. On a later frame, the host calls `kinowasm_resume(store, &args)` once.
5. The guest continues after `next_frame()`; the callback is not called again for that suspended call.

The bridge maps the known signal to `1` (suspended), normal completion to `0`, and failures to `-1`. It resumes only the known signal. It does not treat every error as resumable.

`kinowasm_resume`'s argument array receives the completed export's results. It does **not** replace a suspended host call's return value. A value returned by the callback is captured at suspension. For asynchronous results, use a void wait import, then a separate host import to read completed data after resumption. Do not retain `kinowasm_callinfo_t` or its argument/return pointers past the callback.

Suspension is cooperative: a compute loop that never reaches a yielding import can still block a frame. Each segment must fit your frame budget, and callbacks must start work or check readiness without blocking. The bridge is a **single-session** demonstration using global runtime state: use one instance, one thread, and no interleaved independent invocations. It is not a general engine plugin or a multi-instance scheduler.

## Build and run the native example

Use the repository's Windows x64 toolchain and wabt. From the root:

```bat
cmake --preset x64-Release -DKINORUNTIME_BUILD_EXAMPLES=ON
cmake --build out/build/x64-Release -j 1
ctest --test-dir out/build/x64-Release --output-on-failure
out\build\x64-Release\examples\cooperative\kino_cooperative_demo.exe out\build\x64-Release\examples\cooperative\cooperative.wasm
```

The optional example produces `kino_cooperative.dll`, its import library, the guest module and a console driver under `out/build/<preset>/examples/cooperative/`. It is disabled by default. Use the matching Debug preset/path for a Debug build.

| Host frame | Guest work | Result |
|---|---|---|
| 1 | Reports 10, waits inside a nested call | Suspended |
| 2 | Resumes, reports 20, waits | Suspended |
| 3 | Resumes, reports 30, waits | Suspended |
| 4 | Resumes, returns 10 + 20 + 30 | Completed: 60 |

The driver checks these four steps, reopening after completion, and cancellation/reopening while suspended. The bridge copies the input bytes and retains the store/module buffers until `kino_example_close`. The sample budgets are 100 MiB for the store and 20 MiB for the module, plus the reference host's system arena; these are demonstration budgets, not runtime minimums.

## Unity: Update

The [C# component](unity/KinoCooperativeExample.cs) calls the sample's C ABI with `DllImport`, then steps it once per `Update`.

1. Build the native example in Release.
2. Copy `kino_cooperative.dll` into `Assets/Plugins/x86_64/`. In Unity's plugin settings, enable Windows x86_64 Editor/Standalone as appropriate.
3. Copy `cooperative.wasm` into `Assets/StreamingAssets/`.
4. Copy the C# script into the project and attach it to one GameObject.
5. Run the scene; the console should show progress 10, 20, 30 and completion 60. Disabling the component closes/cancels the session; reenabling starts a new session.

This is a Windows desktop integration template. StreamingAssets access and native linking differ on other platforms. Unity Editor/player execution has not been validated here. See Unity's [native plugin documentation](https://docs.unity.com/en-us/engine/6000.0/manual/scripting/compilation-and-code-reload/plug-ins/native/overview).

## Unreal Engine: Tick

The [Actor](unreal/KinoCooperativeActor.h) and [implementation](unreal/KinoCooperativeActor.cpp) use the same C ABI. Copy them into your game module, and copy `cooperative_bridge.h` into its include path. Link the Release import library and stage/load the matching DLL before `BeginPlay`.

For example, arrange the native files under your module's `ThirdParty/KinoCooperative/`, then add these lines inside your existing `.Build.cs` constructor (with `System.IO` available):

```csharp
if (Target.Platform == UnrealTargetPlatform.Win64)
{
    string kino_dir = Path.Combine(ModuleDirectory, "ThirdParty", "KinoCooperative");
    PublicIncludePaths.Add(Path.Combine(kino_dir, "include"));
    PublicAdditionalLibraries.Add(Path.Combine(kino_dir, "lib", "kino_cooperative.lib"));
    PublicDelayLoadDLLs.Add("kino_cooperative.dll");
    RuntimeDependencies.Add("$(TargetOutputDir)/kino_cooperative.dll",
        Path.Combine(kino_dir, "bin", "kino_cooperative.dll"));
}
```

Put the header in `include/`, the import library in `lib/`, and the DLL in `bin/`. For Editor use, also put the DLL beside the Editor target under the project's `Binaries/Win64/` or load it explicitly from your module startup. Put the guest file at `Content/KinoWASM/cooperative.wasm`, place one Actor in a level and run PIE. It invokes/resumes once per `Tick` and closes on `EndPlay`.

The Actor reads a loose file. For packaged games, stage the guest as a non-asset file or adapt the loader to the project's asset pipeline. Unreal Build Tool, PIE and packaged builds have not been validated here. See Epic's [Actor ticking](https://dev.epicgames.com/documentation/en-us/unreal-engine/actor-ticking-in-unreal-engine) and [third-party library integration](https://dev.epicgames.com/documentation/en-us/unreal-engine/integrating-third-party-libraries-into-unreal-engine) documentation.

These samples illustrate integration with each engine's main loop; they do not claim official Unity/Unreal support or comparison with every other WebAssembly runtime. Start with the [embedding guide](../../docs/QuickGuide.md) when adapting the host backend and lifecycle to a product.
