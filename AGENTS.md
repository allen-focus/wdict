# AGENTS.md — OpenCode instructions

Do not stage or commit this file.

## Build

```bash
cmake -B build -G "Ninja Multi-Config"
cmake --build build --config debug    # or: profile, release
```

- MSVC only, x64 only (CMakeLists.txt:28-34).
- All `.c` files at repo root are auto-globbed — after adding a new `.c`, re-run `cmake -B build`.
- `thirdparty/zstd/*/*.c` is compiled as a separate static library target (`zstd`) linked into `ui`. No per-file agent action needed.
- `build/` is output dir for all configs.
- Alternative one-shot: `build.bat` (uses `cl.exe` directly, auto-detects vswhere).
- No automated tests exist.
- **`ui.exe` may linger after a crash**; if `LNK1168: cannot open ui.exe for writing`, run `taskkill /f /im ui.exe`.

## Entry point quirk

Debug/Profile use normal `wWinMain`. Release skips CRT startup via `WinMainCRTStartup` (no C runtime init).

## State model

```
AppShared (global, one copy)
  ├── dwrite, fonts[FONT_CAPACITY], raster_cache (shared glyph cache)
  ├── renderer_shared (D3D11 device, swapchain factory)
  ├── theme (light/dark, toggled at runtime), accent_border_color, has_accent_border
  ├── cmd_registry (cfg_arena), shortcut_registry (cfg_arena), cmd_queue (cmd_arena)
  ├── palette_search (SearchState — multi-mode, used by quick-search palette)
  ├── dict_db (DictDB, zero-copy view of decompressed dict)
  ├── dict_arena (Arena — decompressed dict.bin.zstd lives here)
  ├── dict_ready (volatile b32 — True once dict + search fully initialized)
  ├── search_aux (DictSearchAuxEntry*), search_aux_arena
  ├── quick_search_window (WindowContext* — separate hidden window for Ctrl+Alt+M palette)
  ├── last_active_main_window, tray_window, tray_hwnd, main_thread_id
  ├── cross_drag_active, cross_drag_source_window_id, cross_drag_payload_buf[DRAG_PAYLOAD_MAX]
  ├── drag_popup (DragPopup*)
  └── WindowContext linked list (first_window → last_window)

WindowContext (per-window)
  ├── HWND, root_panel*, focused_panel, ui (UIContext), renderer
  ├── dcomp_device/target/visual (DirectComposition)
  ├── in_frame (re-entrance guard), mouse_tracked
  ├── ui.requested_frames (idle wake counter)
  ├── tb_hovered_button, decoration_{minimize,maximize,close,menu,search} rects + spacer_rects
  ├── menu_popup, palette_popup (two overlay popups)
  ├── palette_text_buf/edit, palette_selected_index, palette_activate_pending
  ├── palette_search_mode / palette_effective_mode (UI vs worker field config)
  ├── palette_switch_version (published_version gate for LOADING state)
  ├── is_quick_search, quick_search_confirmed_word_idx
  ├── dict_pos_count/kinds/cur_pos, dict_content_active
  ├── dict_token_arena, dict_word_blocks/token arrays
  └── default_himc (IME context)
```

## Search palette (Ctrl+Alt+M hotkey, also search button in title bar)

Centered 500×380 overlay with multi-mode fuzzy search against word/definition/example fields. Runs in a hidden `quick_search_window` that is shown/hidden on toggle. The `palette.close` command (`Ctrl+[`) closes it; pressing `Ctrl+Alt+M` again when focused toggles it closed.

| Shortcut | Action | Conflict handling |
|----------|--------|-------------------|
| Ctrl+Alt+M | Toggle palette (global hotkey) | None |
| Ctrl+D | Switch to Definition mode | Consumed before shortcut lookup |
| Ctrl+E | Switch to Example mode | Consumed before shortcut lookup |
| Ctrl+W | Switch to Word mode | Consumed → tab.close NOT triggered |
| Tab / Shift+Tab | Cycle mode forward / backward | Only when palette text field focused |
| Up/Down/Enter | Navigate / confirm | Only when palette text field focused |
| Ctrl+P/N | Navigate up/down | Only when palette text field focused |
| Ctrl+[ | Close palette | `palette.close` command |
| Esc | Close overlay, then hide window | Falls through to DestroyWindow only if palette already closed |
| Alt+F4 | Close palette (quick-search) / DestroyWindow (main) | |

**Mode switching flow** (`WM_KEYDOWN` before shortcut lookup):
1. `search_reconfigure(&palette_search, fields, count)` — suspends workers, swaps `active_fields`, resumes
2. Re-issues `search_set_query` with current query text
3. Sets `palette_switch_version` so UI shows LOADING until new results arrive

### Palette display items

`palette_build_display_items()` converts raw `SearchResult[]` → `PaletteDisplayItem[]`:
- WORD mode: `word_text` = headword, `word_ranges` = highlight offsets, `display_context` = first `brief_zh` entry (no context highlights).
- DEF/EXAMPLE modes: `word_text` = headword, `display_context` = matching definition/example snippet, `ctx_ranges` = segment-local normalized highlight offsets.
- Uses `aux_segment_locate()` (binary search over `AuxSegment[]`) to find which semantic segment contains the match.
- UTF-8 boundary snapping (`utf8_back_to_char_start` / `utf8_fwd_to_char_end`) prevents mid-codepoint clipping.

### Context truncation

`PALETTE_CONTEXT_MAX_CHARS` (64) limits def/example display to ~2 lines. Ranges past the truncation point are dropped or clipped. Truncated context ends with "…".

## Fonts (FONT_INDEX_*)

| Index | Font | Source |
|-------|------|--------|
| UI | Segoe UI | system |
| ZH | Microsoft YaHei | system |
| MONO | Consolas | system |
| MDL | Segoe MDL2 Assets | system (icon glyphs) |
| ICON | ICON_FONT | embedded resource (Fontello icons) |

## Key shortcuts

| Keys | Command |
|------|---------|
| Ctrl+T | tab.new |
| Ctrl+W | tab.close (unless palette is focused — then palette word mode) |
| Ctrl+Shift+N | window.create (new window) |
| Ctrl+Shift+W | window.destroy |
| F1 | menu.toggle |
| Alt+Shift+- | panel.split_h |
| Alt+Shift+= | panel.split_v |
| Ctrl+Shift+H/L | tab.activate_left/right |
| Ctrl+Shift+Alt+H/L | tab.move delta=-1/+1 |
| Ctrl+Shift+F | tab.to_new_panel axis=X |
| Ctrl+Shift+G | tab.to_new_panel axis=Y |
| Ctrl+Tab | panel.focus_next |
| Ctrl+Shift+Tab | panel.focus_prev |
| Ctrl+Alt+H/J/K/L | panel.focus_left/down/up/right |
| Alt+Shift+H/J/K/L | panel.resize_left/down/up/right |
| Alt+H/L | dict.pos_select delta=-1/+1 (next/prev part of speech) |
| F11 | app.toggle_theme |
| Ctrl+Alt+M | quick search palette toggle |
| Ctrl+[ | palette.close |
| Ctrl+D/E/W | palette mode switch (when palette focused) |
| Esc | Close overlays, then DestroyWindow |
| Alt+F4 | Close palette (quick-search window) or DestroyWindow (main) |

## Commands registered

`window.create`, `app.toggle_theme`, `menu.toggle`, `palette.close`, `panel.split_h/v`, `panel.close`, `tab.new/close/activate/move/move_to_panel/move_to_new_window/to_new_panel`, `panel.focus_next/prev/left/down/up/right`, `panel.resize_left/down/up/right`, `dict.pos_select`, `tab.open_word`.

## Overlay click-through prevention (mouse exile)

Two popups in `popups[]`: menu_popup, palette_popup.

At the top of `process_frame`, check if any open overlay's cached `rect` covers the mouse. If yes, **exile the mouse to (-FLT_MAX, -FLT_MAX)** and clear click/press/delta flags before the panel pass. Restore before overlay rendering.

```
process_frame:
  1. Input pre-pass: open overlay rect contains mouse? → overlay_claims_input = True
  2. Save 8 mouse fields, exile to (-FLT_MAX, -FLT_MAX), clear click/press/delta
  3. panel_container(...)   // all boxes see in_box=False
  4. Restore 8 mouse fields
  5. decoration_overlay()
  6. search_palette_render()
```

To add a new overlay: (1) `OverlayPopup` in `WindowContext`, (2) cache `rect` from `ir.last_box`, (3) add to `popups[]` in pre-pass, (4) add to `WM_LBUTTONDOWN` outside-click close, (5) add to `VK_ESCAPE` close loop.

## Search engine (search.h / search.c)

One `SearchState` instance:
- `palette_search` — starts as word-only; `search_reconfigure()` swaps `active_fields` between 3 FieldDef sets (word / def / example)

### FieldDef arrays

```c
s_dict_fields     = [{ "word", dict_word_extract, 1.0f }]
s_dict_fields_def = [{ "def",  dict_def_extract,  1.0f }]
s_dict_fields_ex  = [{ "ex",   dict_ex_extract,   1.0f }]
```

### search_reconfigure

Suspends all workers (join), swaps `active_fields`/`active_field_count`, invalidates cached query (`query_len = -1` to bypass dedup), resumes workers. Called from render (mode_changed) and WM_KEYDOWN (Ctrl+D/E/W). Does NOT destroy events or worker_results — `search_stop` / `search_start` can't be reused for field swaps because `search_stop` frees all resources.

### DictSearchAuxEntry / AuxSegment

Pre-built at startup via `dict_build_search_aux()` (arena ~20 MB):
- `def_search_text`: all EN+ZH definitions concatenated with `\x01` separators
- `ex_search_text`: all EN+ZH examples concatenated
- Each string has an `AuxSegment[]` table for reverse-mapping byte offsets to individual sentences
- Global: `g_dict_db`, `g_search_aux`

## Dictionary data flow (zstd compression + lazy loading)

`data/dict.bin.zstd` (~6 MB, LZ19) is embedded as `DICT_DATA RCDATA` via `resource.rc`. At startup, `startup_dict_thread` background-decompresses it into `dict_arena`, then builds search aux data. The window shows immediately with a centered "Loading dictionary…" screen if the thread hasn't finished yet.

```
resource.rc → .exe (DICT_DATA RC resource)
  → startup_dict_thread: FindResource → LockResource → ZSTD_getFrameContentSize
  → arena_new(dsize) + arena_push (commit all pages before writing)
  → ZSTD_decompress → dict_open → dict_build_search_aux → search_init/start
  → shared.dict_ready = True → PostMessage(WM_NULL) wakes the message loop
```

### Binary format (v3)

```
[FileHeader     32 B]  magic="DICT", version=3, word_count, section offsets, variant_off, variant_count
[WordIndex[]   12 B]   word_stroff, entdata_off, freq
[EntryData    variable] tightly-packed EntryBlobs
[VariantIndex  8 B]    variant_stroff, base_word_idx  (sorted, 0 entries if none)
[StringPool   variable] all UTF-8 null-terminated strings
```

`dict_lookup()` does case-insensitive binary search on WordIndex. `dict_resolve()` first tries `dict_lookup()`, then falls back to binary search on VariantIndex for variant→base-word resolution. Used in `dict_build_tokens_for_block()` so clicking inflected words (e.g. "apples") jumps to the base word entry.

`volatile b32 AppShared.dict_ready` gates:
- `quick_search_activate` — silently ignored while loading
- `process_frame()` — skips `panel_container`, renders loading text when `!dict_ready`
- Extract callbacks (`dict_word/def/ex_extract`) have NULL guards as safety net

**Release note**: `ZSTD_decompress()` internally allocates a `ZSTD_DCtx` (~200 KB) via `malloc`. Release build links `ucrt` + `libvcruntime` (`#pragma comment` in `main.c:4949-4950`), so `malloc` works.

## Startup flow

```
wWinMain:
  1. init cmds / shortcuts / cursors
  2. spawn startup_dict_thread (detached, no wait)
  3. sync init dwrite → fonts → D3D11 (former startup_raster_renderer_thread, now inline)
  4. create_window (warmup frames may show loading)
  5. ShowWindow → message loop
```

## Architecture

C99 immediate-mode GUI: D3D11, DirectWrite, flexbox layout, single draw call per frame.

Layers:
```
utils → cdwrite.h → LRU → glyph_cache → renderer
       ↓
       ui (box/layout/interaction/widgets) → cmd → shortcut → panel
       ↓
       main.c (entry, WndProc, decoration overlay, palette, message loop)
```

## Key patterns

### Hash string IDs (`##` / `###`)
```c
"hello##world"   // display "hello", hash key "hello world"
"hello###world"  // display "hello", hash key "world" (full override)
```
- Use panel `id` (stable `u32`), NOT `%p` (pointer) in hash keys.
- `str_fmt(buf_size, fmt, ...)` — stack-allocates `u8[buf_size]`, zero-filled. Use for dynamic hash keys.

### `str()` macro trap
**Never** `str(cond ? "a" : "b")` — ternary decays to `const char*`. Use `cond ? str("a") : str("b")`.

### Arena memory
- `arena_new(cap)`, `arena_push(arena, size, _Alignof(T), count)`.
- **Use `_Alignof`**, not `alignof`.
- Frame lifecycle: backup at `ui_frame_begin`, restore via `arena_pop_to` at `ui_frame_end`.
- `drag_payload_buf` is `u8[64]` in `UIContext`, NOT arena-allocated (outlives frame).

### Types
`u8..u64`, `i8..i64`, `f32/f64`, `byte`, `isize`, `usize`, `b32` (int32_t bool). `True`/`False`.

### Sizing
`fixed(v)`, `fit({.min, .max})`, `fit_grow({...})`, `grow({...})` — per axis.

### Box variable scope
`ui_box(...)` macro defines local `box`. `ui_box_begin()` directly returns `UIBox*`.

### `Assert(0)` for unreachable states
Use `Assert(0)` (__debugbreak), never silent return.

## Agent Traps

### Highlight text wrapping
`render_highlighted_text` makes multiple `ui_text()` calls (normal/highlight/normal segments). In a vertical layout box each call starts a new line. Always wrap `render_highlighted_text` calls in a `LAYOUT_LEFT_TO_RIGHT` child box to keep them on one line.

### UTF-8 range clipping
When normalizing ranges (subtracting `seg->offset` or clamping to `seg->len`), byte-level clipping may land in the middle of a multi-byte CJK character. Always snap with `utf8_back_to_char_start` / `utf8_fwd_to_char_end`. Also clamp `local_end` away from `seg->len` (NUL terminator) with `local_end--` when `local_end == seg->len && local_end > 0`.

### Zero-length ranges → D3D11 crash
`render_highlighted_text` must guard highlight segments with `ranges[r].end > ranges[r].start && ranges[r].start < text.len`. Passing a zero-length String or a String whose first byte is `\0` to `ui_text()` triggers `Assert(codepoint)` in the renderer.

### `str()` with ternary
**Never** `str(cond ? "a" : "b")` — always `cond ? str("a") : str("b")`.

### `%p` in hash keys
Pointer addresses change on panel split/merge → LRU cache miss. Use `panel->id` (%u).

### Button hover vs click
Use `tb_hovered_button` for hover, `WM_NCLBUTTONUP` for clicks. NOT `ui_hovered()` / `ui_lclicked()`.

### `UIBox.position` / `.size` is per-frame
Zeroed by arena reset. For cached layout (HTCAPTION rects), use `dr.last_box->position` / `->size`.

### Spacer_inner, not spacer_container
`spacer_container` includes 1px underline. Capture `spacer_inner` for HTCAPTION.

### Overlay click-through
If you add click reactions to any widget in the panel pass, they fire through open overlays unless the overlay's `rect` is in the pre-pass `popups[]`. Both the frame pre-pass AND `WM_LBUTTONDOWN` outside-click logic must be kept consistent.

### Float offset quirk
`BoxFlag_Float` offset is relative to parent's accumulated child position. In `LAYOUT_TOP_TO_BOTTOM` parent, `float_offset.y = -(parent_height)` to cancel prior vertical space.
