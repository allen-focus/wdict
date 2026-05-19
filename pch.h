#pragma once

#pragma comment(lib, "kernel32")
#pragma comment(lib, "user32")
#pragma comment(lib, "d3d11")
#pragma comment(lib, "dxguid")
#pragma comment(lib, "dxgi")
#pragma comment(lib, "d3dcompiler")
#pragma comment(lib, "dwrite")

// Win32
#define COBJMACROS
#include <windows.h>
#include <windowsx.h>
#include <ShellScalingApi.h>

// DirectX
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_3.h>
#include <dxgidebug.h>

#include "cdwrite.h" // IWYU pragma: keep
