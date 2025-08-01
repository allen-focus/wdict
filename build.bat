@echo off
setlocal enabledelayedexpansion
set start=%time%

where /q cl || (
  echo [Error]: "cl" not found - please run this from the MSVC x64 native tools command prompt.
  exit /b 1
)

if "%1"=="-r" (
    set IsRelease=1
) else (
    set IsRelease=0
)

:: -------------------
:: Prepare for project
:: -------------------

:: Create .gitignore if not exist
if not exist ".gitignore" (
    echo /build/ >.gitignore
    echo /shaders/ >>.gitignore
    echo /.cache/ >>.gitignore
    echo *.10x >>.gitignore
    echo *.raddbg_project >>.gitignore
    echo *.raddbg_user >>.gitignore
)

:: Set build directory and output binary
for /f %%q in ("%~dp0.") do set BinaryName=%%~nxq
set BuildDir=build
set BinaryPath=%BuildDir%\%BinaryName%.exe
if not exist %BuildDir% (mkdir %BuildDir%)

:: Kill previous process
tasklist | find "%BinaryName%.exe" >nul && taskkill /F /IM %BinaryName%.exe 2>nul

:: --------------------
:: Compile source files
:: --------------------

:: Select compiler
where clang-cl >nul 2>&1
if errorlevel 1 (
    set "Compiler=cl"
) else (
    set "Compiler=clang-cl"
)
:: Note: We use cl as default for speed
::set Compiler=cl

:: Set compiler and linker flags
:: TODO: /fsanitize=address
set CommonCompilerFlags=/nologo /W3 /WX
if "%Compiler%"=="clang-cl" (
    set CommonCompilerFlags=%CommonCompilerFlags% -Wsign-conversion -Wno-unused-variable -fansi-escape-codes
) else (
    set CommonCompilerFlags=%CommonCompilerFlags% /MP
)
set CompilerReleaseFlags= /O2 /DNDEBUG
set CompilerDebugFlags=/Od /Zi /RTC1

set LinkerReleaseFlags=/incremental:no /opt:icf /opt:ref
set LinkerDebugFlags=

if %IsRelease%==1 (
    set CompilerFlags=%CommonCompilerFlags% %CompilerReleaseFlags%
    set LinkerFlags=%LinkerReleaseFlags%
) else (
    set CompilerFlags=%CommonCompilerFlags% %CompilerDebugFlags%
    set LinkerFlags=%LinkerDebugFlags%
)

:: Set source files
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

:: Compile
set CompileCommand=%Compiler% %CompilerFlags% %SourceFiles% /Fe:%BinaryPath% /Fo:%BuildDir%\ /Fd:%BuildDir%\ /D_CRT_SECURE_NO_WARNINGS
if exist "main.manifest" (
    %CompileCommand% /link %LinkerFlags% /MANIFEST:EMBED /MANIFESTINPUT:main.manifest
) else (
    %CompileCommand% /link %LinkerFlags%
)

:: ----------------------------------------------------
:: Generate compile_commands.json (for clangd analysis)
:: ----------------------------------------------------

set "CompileCommandsJson=%CD%\%BuildDir%\compile_commands.json"
echo [ > %CompileCommandsJson%
set first=1
for %%f in (*.c) do (
    if !first!==1 (
        set first=0
    ) else (
        echo , >> %CompileCommandsJson%
    )
    echo   { >> %CompileCommandsJson%
    echo     "directory": "%CD:\=\\%", >> %CompileCommandsJson%
    echo     "command": "%CompileCommand:\=\\%", >> %CompileCommandsJson%
    echo     "file": "%CD:\=\\%\\%%f" >> %CompileCommandsJson%
    echo   } >> %CompileCommandsJson%
)
echo ] >> %CompileCommandsJson%

:: --------------------
:: Calculate time taken
:: --------------------

set end=%time%
set options="tokens=1-4 delims=:.,"
for /f %options% %%a in ("%start%") do set /a start_m=100%%b%%100&set /a start_s=100%%c%%100&set /a start_ms=100%%d%%100
for /f %options% %%a in ("%end%") do set /a end_m=100%%b%%100&set /a end_s=100%%c%%100&set /a end_ms=100%%d%%100

:: Convert all to milliseconds first
set /a start_total_ms=(start_m * 60 * 1000) + (start_s * 1000) + start_ms
set /a end_total_ms=(end_m * 60 * 1000) + (end_s * 1000) + end_ms

:: Calculate difference
set /a diff_ms=end_total_ms-start_total_ms
set /a diff_s=diff_ms/1000
set /a diff_ms_remain=diff_ms%%1000
echo Time taken: %diff_s%.%diff_ms_remain% seconds

:: ------------------------
:: Check success or failure
:: ------------------------

if exist "%BinaryPath%" (
    echo [Info]: Build successfully
    exit /b 0
) else (
    echo [Error]: Build failed
    exit /b 1
)
