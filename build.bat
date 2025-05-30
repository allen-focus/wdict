@echo off
setlocal enabledelayedexpansion
set start=%time%

where /q cl || (
  echo Error: "cl" not found - please run this from the MSVC x64 native tools command prompt.
  exit /b 1
)

:: Create .gitignore
if not exist ".gitignore" (
    echo /build/ >.gitignore
    echo /shaders/ >>.gitignore
    echo /.cache/ >>.gitignore
)

:: Check if open_source.c exists
if exist "open_source.c" (
    echo Info: `open_source.c` exist. Archive HEAD repo to `source.zip`...
    git archive -o source.zip HEAD
    if errorlevel 1 (
        echo Error: No git repo
        del source.zip
        exit /b 1
    )
)

:: Determine release/debug mode
if "%1"=="-r" (
    set IsRelease=1
    echo "Build in release mode"
) else (
    set IsRelease=0
)

:: Set root directory name as exe name
for /f %%q in ("%~dp0.") do set ExeName=%%~nxq

:: Kill previous process
tasklist | find "%ExeName%.exe" >nul && taskkill /F /IM %ExeName%.exe 2>nul

:: --- Set and clean build and shaders directory (using ping as sleep) ---
set BuildDir=build
if exist %BuildDir% (
    ping 127.0.0.1 -n 1 -w 200 > nul
    rmdir /s /q %BuildDir%
)
mkdir %BuildDir%

set ShadersDir=shaders
if exist %ShadersDir% (
    ping 127.0.0.1 -n 1 -w 200 > nul
    rmdir /s /q %ShadersDir%
)
mkdir %ShadersDir%

:: Set source files
set SourceFiles=
for %%f in (*.c) do (
    set SourceFiles=!SourceFiles! %%f
)

:: Compile resource if exists
if exist "resource.rc" (
    rc /nologo /fo %BuildDir%\resource.res resource.rc
    set SourceFiles=%SourceFiles% %BuildDir%\resource.res
)

set OutputExe=%BuildDir%\%ExeName%.exe

:: Choose compiler
::where clang-cl >nul 2>&1
::if errorlevel 1 (
::    where cl >nul 2>&1
::    if errorlevel 1 (
::        echo No suitable compiler found. Please install Visual Studio Build Tools or clang-cl.
::        exit /b 1
::    ) else (
::        set "Compiler=cl"
::    )
::) else (
::    set "Compiler=clang-cl"
::)
:: Note: We use cl as default for speed
set Compiler=cl

:: Set compiler flags based on chosen compiler
if "%Compiler%"=="clang-cl" (
    set CommonCompilerFlags=/nologo /W3 -Wsign-conversion -Wno-unused-variable /WX
) else (
    set CommonCompilerFlags=/nologo /W3 /WX
)

::set CompilerDebugFlags=/Od /Zi /RTC1 /fsanitize=address
set CompilerDebugFlags=/Od /Zi /RTC1
set CompilerReleaseFlags= /O2 /DNDEBUG

if %IsRelease%==1 (
    set CompilerFlags=%CommonCompilerFlags% %CompilerReleaseFlags%
) else (
    set CompilerFlags=%CommonCompilerFlags% %CompilerDebugFlags%
)

:: Set base compile command
set BaseCompileCommand=%Compiler% %CompilerFlags% %SourceFiles% /Fe:%OutputExe% /Fo:%BuildDir%\ /Fd:%BuildDir%\ /D_CRT_SECURE_NO_WARNINGS

:: Generate compile_commands.json (for clangd analysis)
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
    echo     "command": "%BaseCompileCommand:\=\\%", >> %CompileCommandsJson%
    echo     "file": "%CD:\=\\%\\%%f" >> %CompileCommandsJson%
    echo   } >> %CompileCommandsJson%
)

echo ] >> %CompileCommandsJson%


:: Set linker flags
set LinkerDebugFlags=
set LinkerReleaseFlags=/incremental:no /opt:icf /opt:ref

if %IsRelease%==1 (
    set LinkerFlags=%LinkerReleaseFlags%
) else (
    set LinkerFlags=%LinkerDebugFlags%
)

:: Compile source using chosen compiler
if exist "shader.hlsl" (
    fxc.exe /nologo /T vs_5_0 /E vs /O3 /WX /Zpc /Ges /Fh %ShadersDir%/d3d11_vshader.h /Vn d3d11_vshader /Qstrip_reflect /Qstrip_debug /Qstrip_priv shader.hlsl
    fxc.exe /nologo /T ps_5_0 /E ps /O3 /WX /Zpc /Ges /Fh %ShadersDir%/d3d11_pshader.h /Vn d3d11_pshader /Qstrip_reflect /Qstrip_debug /Qstrip_priv shader.hlsl
)

if exist "main.manifest" (
    %BaseCompileCommand% /link %LinkerFlags% /MANIFEST:EMBED /MANIFESTINPUT:main.manifest
) else (
    %BaseCompileCommand% /link %LinkerFlags%
)

:: Calculate time taken
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

:: Check success or failure
if exist "%OutputExe%" (
    echo Info: Build successfully
    exit /b 0
) else (
    echo Error: Build failed
    exit /b 1
)
