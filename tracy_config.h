#pragma once

/*
 * Tracy subsystem bitmasks for per-zone compile-time control.
 *
 * Usage:
 *   TracyCZoneNC(ctx, "Name", TracyColor_Layout, TRACY_SUBSYSTEMS & TracySys_Layout);
 *
 * Define TRACY_SUBSYSTEMS in CMake (Profile config) or let the default apply.
 * A bit set to 1 enables that subsystem's zones; 0 strips them entirely
 * (no branch, no runtime cost).
 *
 * Colors are 0xRRGGBB — use named TracyColor_* constants below for consistency.
 */

/* subsystem colors (0xRRGGBB) */
#define TracyColor_Frame       0x4488FF
#define TracyColor_Layout      0x44CC44
#define TracyColor_Render      0xFF8844
#define TracyColor_Text        0x44DDDD
#define TracyColor_Cache       0x88AAFF
#define TracyColor_Glyph       0xCC44CC
#define TracyColor_Panel       0xDDCC44
#define TracyColor_Scroll      0xFF88AA
#define TracyColor_Widget      0xFFCC44
#define TracyColor_Interaction 0xFF6644
#define TracyColor_Cmd         0x88FF88
#define TracyColor_Drag        0xCC88FF

enum
{
    TracySys_Frame       = 1 << 0,  // ProcessFrame, MainLoop, FrameBegin/End
    TracySys_Layout      = 1 << 1,  // CalcLayout, FitAxis, GrowShrink, ResolvePos, Wrap, TextWrap
    TracySys_Render      = 1 << 2,  // FlushPresent, DrawText, DrawRect, GenRenderCmds, MapVertexBuf
    TracySys_Text        = 1 << 3,  // TextWidth, UIText
    TracySys_Cache       = 1 << 4,  // BoxFind, LRUFindOrEvict, BoxCacheCleanup, RasterCache
    TracySys_Glyph       = 1 << 5,  // RasterizeGlyph, GlyphAtlas
    TracySys_Panel       = 1 << 6,  // PanelBegin/End, PanelBounds, PendingRemoves, TabsCleanup, CalcRect
    TracySys_Scroll      = 1 << 7,  // Scrollbar, SABegin/End
    TracySys_Widget      = 1 << 8,  // Button, TextField, Switchbox
    TracySys_Interaction = 1 << 9,  // Interaction
    TracySys_Cmd         = 1 << 10, // CmdExec
    TracySys_Drag        = 1 << 11, // DragPopupRender/Update, CrossWindowSync
};

#ifndef TRACY_SUBSYSTEMS
/*
 * Default: enable Frame + Layout + Render + Text + Panel + Cmd + Drag.
 * Cache, Glyph, Scroll, Widget, Interaction are off — enable them when
 * profiling specific subsystems.
 */
#    define TRACY_SUBSYSTEMS                                          \
        (TracySys_Frame | TracySys_Layout | TracySys_Render           \
         | TracySys_Text | TracySys_Panel | TracySys_Cmd | TracySys_Drag)
#endif
