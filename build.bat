:: This is a simple build script. 
:: For more advanced configuration options, please use CMake and refer to the CMakeLists.txt file.
::
:: Usage:
::   `build` (equivalent to `build release`)
::   `build release`
::   `build debug`

@echo off
setlocal enabledelayedexpansion

set ARGS=%*
if "%ARGS:release=%" neq "!ARGS!" (
    set IsRelease=1
    echo === Build in release mode ===
) else (
    echo === Build in debug mode ===
    set IsRelease=0
)

:: ---------------------------------------------------------
:: Detect compiler
:: ---------------------------------------------------------

set Compiler=cl

if "%PROCESSOR_ARCHITECTURE%" equ "AMD64" (
  set HOST_ARCH=x64
) else if "%PROCESSOR_ARCHITECTURE%" equ "ARM64" (
  set HOST_ARCH=arm64
)

if "%ARGS:x64=%" neq "!ARGS!" (
  set TARGET_ARCH=x64
) else if "%ARGS:arm64=%" neq "!ARGS!" (
  set TARGET_ARCH=arm64
) else (
  set TARGET_ARCH=%HOST_ARCH%
)

where /Q cl.exe || (
  set __VSCMD_ARG_NO_LOGO=1
  for /f "tokens=*" %%i in ('"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.VisualStudio.Workload.NativeDesktop -property installationPath') do set VS=%%i
  if "!VS!" equ "" (
    echo ERROR: Visual Studio installation not found
    exit /b 1
  )
  call "!VS!\Common7\Tools\VsDevCmd.bat" -arch=%TARGET_ARCH% -host_arch=%HOST_ARCH% -startdir=none -no_logo || exit /b 1
)

:: ---------------------------------------------------------
:: Set compiler and linker flags
:: ---------------------------------------------------------

:: NOTE: Comment sanitize flag as it is too slow
:: set CompilerDebugFlags=/Od /Zi /RTC1 /fsanitize=address

set CommonCompilerFlags=/nologo /W3 /WX /MP /D_CRT_SECURE_NO_WARNINGS
set CompilerDebugFlags=/Od /Zi /RTC1
set CompilerReleaseFlags=/O2 /GS- /DNDEBUG

set LinkerReleaseFlags=/fixed /incremental:no /opt:icf /opt:ref /subsystem:windows
set LinkerDebugFlags=

if %IsRelease%==1 (
    set CompilerFlags=%CommonCompilerFlags% %CompilerReleaseFlags%
    set LinkerFlags=%LinkerReleaseFlags%
) else (
    set CompilerFlags=%CommonCompilerFlags% %CompilerDebugFlags%
    set LinkerFlags=%LinkerDebugFlags%
)

:: ---------------------------------------------------------
:: Set source files
:: ---------------------------------------------------------

set SourceFiles=
for %%f in (*.c) do (
    set SourceFiles=!SourceFiles! %%f
)
if exist "resource.rc" (
    rc /nologo /fo %BuildDir%\resource.res resource.rc
    set SourceFiles=%SourceFiles% %BuildDir%\resource.res
)
if exist "shader.hlsl" (
    if not exist shaders (mkdir shaders)
    fxc.exe /nologo /T vs_5_0 /E vs /O3 /WX /Zpc /Ges /Fh shaders/d3d11_vshader.h /Vn d3d11_vshader /Qstrip_reflect /Qstrip_debug /Qstrip_priv shader.hlsl
    fxc.exe /nologo /T ps_5_0 /E ps /O3 /WX /Zpc /Ges /Fh shaders/d3d11_pshader.h /Vn d3d11_pshader /Qstrip_reflect /Qstrip_debug /Qstrip_priv shader.hlsl
)

:: ---------------------------------------------------------
:: Compile
:: ---------------------------------------------------------

:: Set build directory and output binary
for /f %%q in ("%~dp0.") do set BinaryName=%%~nxq
set BuildDir=build
set BinaryPath=%BuildDir%\%BinaryName%.exe
if not exist %BuildDir% (mkdir %BuildDir%)

set CompileCommand=%Compiler% %CompilerFlags% %SourceFiles% /Fe:%BinaryPath% /Fo:%BuildDir%\ /Fd:%BuildDir%\
if exist "main.manifest" (
    %CompileCommand% /link %LinkerFlags% /MANIFEST:EMBED /MANIFESTINPUT:main.manifest
) else (
    %CompileCommand% /link %LinkerFlags%
)

:: ---------------------------------------------------------
:: Verify build completion
:: ---------------------------------------------------------

if errorlevel 1 (
    echo === Build failed! ===
    exit /b 1
) else (
    echo === Build successfully, see %BinaryPath% ===
    exit /b 0
)
