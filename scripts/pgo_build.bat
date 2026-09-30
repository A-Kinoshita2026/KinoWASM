@echo off
REM ===========================================================================
REM  MSVC PGO build for KinoRuntime, with value-probe removal (pgo_deprobe).
REM
REM  Why pgo_deprobe:
REM    core's dispatch is direct-threaded + musttail and keeps pc/sp/mem/r0 in
REM    rcx/rdx/r8/r9 across handlers. MSVC /GENPROFILE injects "value probes"
REM    (call __PogoProbe*Value*) whose call clobbers those registers, so the
REM    instrumented exe crashes with 0xC0000005. pgo_deprobe NOPs out only the
REM    value-probe calls; the edge-count `inc [r12+off]` records are left intact,
REM    so the instrumented exe runs and emits a correct .pgc. USE then optimizes
REM    from those counts (measured CoreMark ~+11% over non-PGO).
REM
REM  Flow:  [GEN+/MAP build] -> [pgo_deprobe: NOP value probes] -> [train x3]
REM         -> [pgomgr /merge] -> [USE build] -> [copy to Bin]
REM
REM  Usage: scripts\pgo_build.bat [builddir] [coremark.wasm] [ADVENTURE ON|OFF]
REM    builddir       default: <repo>\out\build\x64-Release-pgo
REM    coremark.wasm  default: <repo>\Bin\coremark.wasm  (training workload)
REM    ADVENTURE      default: ON  (matches the normal Release build)
REM ===========================================================================
setlocal
set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvars64.bat"
set "SRC=%~dp0.."
set "BUILD=%~1"
if "%BUILD%"=="" set "BUILD=%SRC%\out\build\x64-Release-pgo"
set "CM=%~2"
if "%CM%"=="" set "CM=%SRC%\Bin\coremark.wasm"
set "ADV=%~3"
if "%ADV%"=="" set "ADV=ON"
set "DEPROBE_SRC=%SRC%\tools\pgo_deprobe\pgo_deprobe.c"
set "DEPROBE_EXE=%SRC%\tools\pgo_deprobe\pgo_deprobe.exe"

call "%VCVARS%" >nul 2>&1 || (echo vcvars64 failed & exit /b 1)

echo [0/9] clean stale build state (exe deleted too, so GEN relinks every exe)
del /q "%BUILD%\*.pgd" "%BUILD%\*.pgc" "%BUILD%\*.exe" "%BUILD%\*.map" 2>nul

echo [1/9] configure GEN (instrument + /MAP for pgo_deprobe)
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl ^
  -DKINOWASM_ENABLE_ADVENTURE=%ADV% -DKINOWASM_PGO_GEN=ON -DKINOWASM_PGO_USE=OFF ^
  -S "%SRC%" -B "%BUILD%" || exit /b 1

echo [2/9] build instrumented (serial: avoids Bin\ copy race)
cmake --build "%BUILD%" -j 1 || exit /b 1

echo [3/9] copy pgort runtime next to instrument exe + build pgo_deprobe
copy /y "%VCToolsInstallDir%bin\Hostx64\x64\pgort*.dll" "%BUILD%\" >nul || (echo pgort copy failed & exit /b 1)
cl /nologo /utf-8 /O2 /Fe:"%DEPROBE_EXE%" /Fo:"%TEMP%\pgo_deprobe.obj" "%DEPROBE_SRC%" >nul || (echo pgo_deprobe build failed & exit /b 1)

echo [4/9] strip value probes from instrument exe (NOP the crashing calls)
"%DEPROBE_EXE%" "%BUILD%\KinoRuntime.exe" "%BUILD%\KinoRuntime.map" || (echo deprobe failed & exit /b 1)

echo [5/9] train: run CoreMark x3 (patched instrument; .pgc lands next to .pgd)
pushd "%BUILD%"
for /L %%i in (1,1,3) do "%BUILD%\KinoRuntime.exe" "%CM%" | findstr /C:"Iterations/Sec"
popd

echo [5b/9] verify training produced a .pgc
if not exist "%BUILD%\KinoRuntime*.pgc" (
  echo ERROR: no .pgc produced -- patched instrument did not run to completion.
  echo        Check pgo_deprobe NOPed the value probes and pgort*.dll is present.
  exit /b 1
)

echo [6/9] merge .pgc into .pgd (inject training counts)
pushd "%BUILD%"
pgomgr /merge KinoRuntime.pgd || (echo pgomgr merge failed & popd & exit /b 1)
popd

echo [7/9] configure USE (optimize with profile)
cmake -DKINOWASM_PGO_GEN=OFF -DKINOWASM_PGO_USE=ON "%BUILD%" || exit /b 1

echo [8/9] build optimized (serial)
cmake --build "%BUILD%" -j 1 || exit /b 1

echo [9/9] copy PGO exe to Bin\ (later non-PGO/Clang builds overwrite Bin\ otherwise)
copy /y "%BUILD%\KinoRuntime.exe" "%SRC%\Bin\KinoRuntime.exe" >nul || (echo copy to Bin failed & exit /b 1)

echo.
echo PGO build done: "%BUILD%\KinoRuntime.exe" (also copied to Bin\)
echo NOTE: re-running a normal Release or Clang build copies ITS exe over Bin\.
echo       Run this script last, or re-copy the PGO exe afterwards.
endlocal
