# build.ps1 - Build script for Win32/D3D11 C project (x64 only)
#
# Usage:
#   .\build.ps1              # debug   - fast iteration, debugger-friendly
#   .\build.ps1 profile      # profile - optimized + Tracy, trustworthy perf data
#   .\build.ps1 release      # release - ship build, zero overhead

[CmdletBinding()]
param(
    [ValidateSet("debug", "profile", "release")]
    [string]$Mode = "debug"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Exit-Failure([string]$msg)
{
    Write-Host "ERROR: $msg" -ForegroundColor Red
    exit 1
}

# ---------------------------------------------------------------------------
# Locate MSVC via vswhere
# ---------------------------------------------------------------------------

$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere))
{
    Exit-Failure "vswhere.exe not found. Please install Visual Studio."
}

$vsPath = & $vswhere -latest -requires Microsoft.VisualStudio.Workload.NativeDesktop -property installationPath
if (-not $vsPath)
{
    Exit-Failure "No Visual Studio installation with NativeDesktop workload found."
}

$launchScript = Join-Path $vsPath "Common7\Tools\Launch-VsDevShell.ps1"

& $launchScript -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null

# ---------------------------------------------------------------------------
# Flags per build mode
# ---------------------------------------------------------------------------

$commonFlags = @(
    "/nologo", "/W3", "/WX", "/MP", "/utf-8"
    "/D_CRT_SECURE_NO_WARNINGS"
)

$modeCompilerFlags = switch ($Mode)
{
    "debug"
    { @("/Od", "/Zi", "/RTC1") 
    }
    "profile"
    { @("/O2", "/Zi", "/DNDEBUG", "/DTRACY_ENABLE") 
    }
    "release"
    { @("/O2", "/GS-", "/DNDEBUG") 
    }
}

$modeLinkerFlags = switch ($Mode)
{
    "debug"
    { @() 
    }
    "profile"
    { @("/subsystem:windows") 
    }
    "release"
    { @("/fixed", "/incremental:no", "/opt:icf", "/opt:ref", "/subsystem:windows") 
    }
}

# ---------------------------------------------------------------------------
# Build directory
# ---------------------------------------------------------------------------

$buildDir = "build"
if (-not (Test-Path $buildDir))
{ New-Item -ItemType Directory -Path $buildDir | Out-Null 
}

# ---------------------------------------------------------------------------
# Tracy client (C++ translation unit, compiled only for profile builds)
# ---------------------------------------------------------------------------

$extraObjs = [System.Collections.Generic.List[string]]::new()

if ($Mode -eq "profile")
{
    $tracyClient = "thirdparty\tracy\public\TracyClient.cpp"
    if (-not (Test-Path $tracyClient))
    {
        Exit-Failure "Tracy client not found at '$tracyClient'. Check your submodule."
    }

    $tracyObj = "$buildDir\TracyClient.obj"
    Write-Host "Compiling Tracy client..." -ForegroundColor DarkGray

    & cl.exe /nologo /O2 /DNDEBUG /DTRACY_ENABLE /EHsc /c $tracyClient /Fo:$tracyObj
    if ($LASTEXITCODE -ne 0)
    { Exit-Failure "Tracy client compilation failed." 
    }

    $extraObjs.Add($tracyObj)
}

# ---------------------------------------------------------------------------
# Shaders
# ---------------------------------------------------------------------------

if (Test-Path "shader.hlsl")
{
    if (-not (Test-Path "shaders"))
    { New-Item -ItemType Directory -Path "shaders" | Out-Null 
    }

    $fxcBase = @("/nologo", "/O3", "/WX", "/Zpc", "/Ges", "/Qstrip_reflect", "/Qstrip_debug", "/Qstrip_priv")

    & fxc.exe @fxcBase /T vs_5_0 /E vs /Fh shaders/d3d11_vshader.h /Vn d3d11_vshader shader.hlsl
    if ($LASTEXITCODE -ne 0)
    { Exit-Failure "Vertex shader compilation failed." 
    }

    & fxc.exe @fxcBase /T ps_5_0 /E ps /Fh shaders/d3d11_pshader.h /Vn d3d11_pshader shader.hlsl
    if ($LASTEXITCODE -ne 0)
    { Exit-Failure "Pixel shader compilation failed." 
    }
}

# ---------------------------------------------------------------------------
# Resource
# ---------------------------------------------------------------------------

if (Test-Path "resource.rc")
{
    & rc.exe /nologo /fo "$buildDir\resource.res" resource.rc
    if ($LASTEXITCODE -ne 0)
    { Exit-Failure "Resource compilation failed." 
    }
    $extraObjs.Add("$buildDir\resource.res")
}

# ---------------------------------------------------------------------------
# C sources
# ---------------------------------------------------------------------------

$sourceFiles = @(Get-ChildItem -Filter "*.c" | Select-Object -ExpandProperty Name)
if ($sourceFiles.Count -eq 0)
{ Exit-Failure "No .c source files found in current directory." 
}

# ---------------------------------------------------------------------------
# Compile & link
# ---------------------------------------------------------------------------

$binaryName = (Get-Item -Path (Get-Location)).Name
$binaryPath = "$buildDir\$binaryName.exe"

Write-Host "=== Build mode: $Mode ===" -ForegroundColor Cyan

$clArgs = $commonFlags + $modeCompilerFlags + $sourceFiles + $extraObjs.ToArray() +
@("/Fe:$binaryPath", "/Fo:$buildDir\", "/Fd:$buildDir\")

$linkArgs = [System.Collections.Generic.List[string]]::new()
foreach ($flag in $modeLinkerFlags)
{ $linkArgs.Add($flag) 
}
if (Test-Path "main.manifest")
{
    $linkArgs.Add("/MANIFEST:EMBED")
    $linkArgs.Add("/MANIFESTINPUT:main.manifest")
}

if ($linkArgs.Count -gt 0)
{
    $clArgs += @("/link") + $linkArgs.ToArray()
}

& cl.exe @clArgs

# ---------------------------------------------------------------------------
# Result
# ---------------------------------------------------------------------------

if ($LASTEXITCODE -ne 0)
{
    Write-Host "=== Build failed! ===" -ForegroundColor Red
    exit 1
} else
{
    Write-Host "=== Build succeeded  ->  $binaryPath ===" -ForegroundColor Green
    exit 0
}
