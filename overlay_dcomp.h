#pragma once

#include <d3d11.h>
#include <dxgi1_3.h>
#include <windows.h>

#ifdef DCOMP_INITGUID
#include <initguid.h>
#endif

//
// Minimal C-compatible DComp COM interface types
// dcomp.h from the Windows SDK is C++ only, so we define what we need manually.
// Pattern matches cdwrite.h: interface type holds `struct { void* tbl[]; }* v;`
// and methods are called via this->v->tbl[index] with a function-pointer cast.
//
// Vtable indices (0-based, after IUnknown slots 0-2):
//   IDCompositionDevice:  tbl[3]=Commit, tbl[6]=CreateTargetForHwnd, tbl[7]=CreateVisual
//   IDCompositionTarget:  tbl[3]=SetRoot
//   IDCompositionVisual:  tbl[15]=SetContent
//

// clang-format off
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
DEFINE_GUID(IID_IDCompositionDevice, 0xC37EA93A, 0xE7AA, 0x450D, 0xB1, 0x6F, 0x97, 0x46, 0xCB, 0x04, 0x07, 0xF3);

typedef struct IDCompositionDevice { struct { void* tbl[]; }* v; } IDCompositionDevice;
typedef struct IDCompositionTarget { struct { void* tbl[]; }* v; } IDCompositionTarget;
typedef struct IDCompositionVisual { struct { void* tbl[]; }* v; } IDCompositionVisual;

HRESULT WINAPI DCompositionCreateDevice(IDXGIDevice* dxgiDevice, REFIID iid, void** dcompositionDevice);

static inline HRESULT IDCompositionDevice_CreateTargetForHwnd(IDCompositionDevice* this, HWND hwnd, BOOL topmost, IDCompositionTarget** target) { return ((HRESULT (WINAPI*)(IDCompositionDevice*, HWND, BOOL, IDCompositionTarget**))this->v->tbl[6])(this, hwnd, topmost, target); }
static inline HRESULT IDCompositionDevice_CreateVisual(IDCompositionDevice* this, IDCompositionVisual** visual)       { return ((HRESULT (WINAPI*)(IDCompositionDevice*, IDCompositionVisual**))this->v->tbl[7])(this, visual); }
static inline HRESULT IDCompositionDevice_Commit(IDCompositionDevice* this)                                          { return ((HRESULT (WINAPI*)(IDCompositionDevice*))this->v->tbl[3])(this); }
static inline UINT32  IDCompositionDevice_Release(IDCompositionDevice* this)                                        { return ((UINT32  (WINAPI*)(IDCompositionDevice*))this->v->tbl[2])(this); }
static inline HRESULT IDCompositionTarget_SetRoot(IDCompositionTarget* this, IDCompositionVisual* visual)            { return ((HRESULT (WINAPI*)(IDCompositionTarget*, IDCompositionVisual*))this->v->tbl[3])(this, visual); }
static inline UINT32  IDCompositionTarget_Release(IDCompositionTarget* this)                                        { return ((UINT32  (WINAPI*)(IDCompositionTarget*))this->v->tbl[2])(this); }
static inline HRESULT IDCompositionVisual_SetContent(IDCompositionVisual* this, IUnknown* object)                    { return ((HRESULT (WINAPI*)(IDCompositionVisual*, IUnknown*))this->v->tbl[15])(this, object); }
static inline UINT32  IDCompositionVisual_Release(IDCompositionVisual* this)                                        { return ((UINT32  (WINAPI*)(IDCompositionVisual*))this->v->tbl[2])(this); }

#pragma clang diagnostic on
// clang-format on
