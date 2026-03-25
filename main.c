#include "pch.h" // IWYU pragma: keep
#include "glyph_cache.h"
#include "renderer.h"
#include "ui.h"
#include "utils.h"

#include "thirdparty/tracy/public/tracy/TracyC.h"

#include <math.h>
#include <wchar.h>

///

#define CLIENT_WIDTH  800
#define CLIENT_HEIGHT 400

#define MAX_TITLE_LENGTH 64

#define FONT_INDEX_UI 0
#define FONT_INDEX_ZH 1
#define FONT_INDEX_MONO 2
#define FONT_CAPACITY 3

///

typedef struct
{
    wchar_t title[MAX_TITLE_LENGTH];
    UIContext ui;
    IDWriteFactory3* dwrite_factory;
    Font fonts[FONT_CAPACITY];
    GlyphCache glyph_cache;
} AppContext;

///

static void process_frame(AppContext* app_context)
{
    TracyCFrameMark;

    TracyCZone(ctx, 1);

    // Style
    RectStyle background_rect_style = { .border_color = { 0, 0, 0, 0 },         .corner_radius = 0,  .border_thickness = 0, .enable_shadow = False };
    RectStyle normal_rect_style =     { .border_color = { 0, 0, 0, 0 },         .corner_radius = 12, .border_thickness = 0, .enable_shadow = False };
    RectStyle full_rect_style =       { .border_color = { 255, 255, 255, 255 }, .corner_radius = 15, .border_thickness = 5, .enable_shadow = True };

    // Color
    Color black = { 0,   0,   0,   255 };
    Color grey  = { 192, 192, 192, 255 };
    Color white = { 255, 255, 255, 255 };
    Color green = { 0,   255, 0,   255 };
    Color red   = { 255, 0,   0,   255 };
    Color blue  = { 0,   0,   255, 255 };

    // Padding
    Padding padding_bigger  = { 50, 50, 50, 50 };
    Padding padding_big     = { 30, 30, 30, 30 };
    Padding padding_medium  = { 20, 20, 20, 20 };
    Padding padding_small   = { 10, 10, 10, 10 };
    Padding padding_smaller = { 5,  5,  5,  5  };

    // Child gap
    f32 child_gap_bigger  = 30;
    f32 child_gap_big     = 20;
    f32 child_gap_medium  = 10;
    f32 child_gap_small   = 5;
    f32 child_gap_smaller = 3;

    // Alias
    UIContext* ui_context = &app_context->ui;
    GlyphCache* glyph_cache = &app_context->glyph_cache;
    Font* font_ui = &app_context->fonts[FONT_INDEX_UI];
    Font* font_zh = &app_context->fonts[FONT_INDEX_ZH];
    Font* font_mono = &app_context->fonts[FONT_INDEX_MONO];

    ///

    ui_reset(ui_context);

    ui_box({
        .sizing = { fixed((f32)ui_context->client_width), fixed((f32)ui_context->client_height) },
        .color = white,
        .rect_style = background_rect_style,
        .padding = padding_bigger,
        .child_gap = child_gap_bigger,
        .direction = LAYOUT_LEFT_TO_RIGHT
    }) {
        // ─── Left Sidebar ──────────────────────────────────────────────
        ui_box({
            .sizing = { fixed(180), fit_grow({}) },
            .color = grey,
            .rect_style = full_rect_style,
            .padding = padding_medium,
            .child_gap = child_gap_medium,
            .direction = LAYOUT_TOP_TO_BOTTOM
        }) {
            // Top: Avatar + Icon Row
            ui_box({
                .sizing = { fit_grow({}), fit({}) },
                .child_gap = child_gap_small,
                .direction = LAYOUT_LEFT_TO_RIGHT
            }) {
                ui_box({ .sizing = { fixed(44), fixed(44) }, .color = white, .rect_style = normal_rect_style }) {}
                ui_box({
                    .sizing = { fit_grow({}), fit({}) },
                    .padding = padding_smaller,
                    .child_gap = child_gap_smaller,
                    .direction = LAYOUT_TOP_TO_BOTTOM
                }) {
                    ui_box({ .sizing = { fit_grow({}), fixed(18) }, .color = green }) {}
                    ui_box({ .sizing = { fit_grow({}), fixed(18) }, .color = blue }) {}
                }
            }
            // Divider line
            ui_box({ .sizing = { fit_grow({}), fixed(1) }, .color = white }) {}
            // Navigation items ×5
            ui_box({
                .sizing = { fit_grow({}), fixed(36) },
                .color = blue,
                .rect_style = normal_rect_style,
                .padding = padding_small,
                .child_gap = child_gap_small,
                .direction = LAYOUT_LEFT_TO_RIGHT,
                .alignment = { .y = ALIGN_CENTER }
            }) {
                ui_box({ .sizing = { fixed(14), fixed(14) }, .color = green }) {}
                ui_text(ui_context, glyph_cache, str("概览"),
                    &(TextConfig){ .font = font_zh, .font_size = 13, .color = white, .line_height = 16 });
            }
            ui_box({
                .sizing = { fit_grow({}), fixed(36) },
                .color = blue,
                .rect_style = normal_rect_style,
                .padding = padding_small,
                .child_gap = child_gap_small,
                .direction = LAYOUT_LEFT_TO_RIGHT,
                .alignment = { .y = ALIGN_CENTER }
            }) {
                ui_box({ .sizing = { fixed(14), fixed(14) }, .color = red }) {}
                ui_text(ui_context, glyph_cache, str("数据"),
                    &(TextConfig){ .font = font_zh, .font_size = 13, .color = white, .line_height = 16 });
            }
            ui_box({
                .sizing = { fit_grow({}), fixed(36) },
                .color = blue,
                .rect_style = normal_rect_style,
                .padding = padding_small,
                .child_gap = child_gap_small,
                .direction = LAYOUT_LEFT_TO_RIGHT,
                .alignment = { .y = ALIGN_CENTER }
            }) {
                ui_box({ .sizing = { fixed(14), fixed(14) }, .color = white }) {}
                ui_text(ui_context, glyph_cache, str("设置"),
                    &(TextConfig){ .font = font_zh, .font_size = 13, .color = white, .line_height = 16 });
            }
            ui_box({
                .sizing = { fit_grow({}), fixed(36) },
                .color = grey,
                .rect_style = normal_rect_style,
                .padding = padding_small,
                .child_gap = child_gap_small,
                .direction = LAYOUT_LEFT_TO_RIGHT,
                .alignment = { .y = ALIGN_CENTER }
            }) {
                ui_box({ .sizing = { fixed(14), fixed(14) }, .color = green }) {}
                ui_text(ui_context, glyph_cache, str("日志"),
                    &(TextConfig){ .font = font_zh, .font_size = 13, .color = white, .line_height = 16 });
            }
            ui_box({
                .sizing = { fit_grow({}), fixed(36) },
                .color = grey,
                .rect_style = normal_rect_style,
                .padding = padding_small,
                .child_gap = child_gap_small,
                .direction = LAYOUT_LEFT_TO_RIGHT,
                .alignment = { .y = ALIGN_CENTER }
            }) {
                ui_box({ .sizing = { fixed(14), fixed(14) }, .color = red }) {}
                ui_text(ui_context, glyph_cache, str("帮助"),
                    &(TextConfig){ .font = font_zh, .font_size = 13, .color = white, .line_height = 16 });
            }
            // Flexible blank space
            ui_box({ .sizing = { fit_grow({}), fit_grow({}) } }) {}
            // Badge row (ALIGN_END)
            ui_box({
                .sizing = { fit_grow({}), fit({}) },
                .child_gap = child_gap_smaller,
                .direction = LAYOUT_LEFT_TO_RIGHT,
                .alignment = { .x = ALIGN_END, .y = ALIGN_CENTER }
            }) {
                ui_box({ .sizing = { fixed(10), fixed(10) }, .color = green, .rect_style = normal_rect_style }) {}
                ui_box({ .sizing = { fixed(10), fixed(10) }, .color = red,   .rect_style = normal_rect_style }) {}
                ui_box({ .sizing = { fixed(10), fixed(10) }, .color = blue,  .rect_style = normal_rect_style }) {}
            }
            // Small icon row
            ui_box({
                .sizing = { fit_grow({}), fit({}) },
                .child_gap = child_gap_small,
                .direction = LAYOUT_LEFT_TO_RIGHT
            }) {
                ui_box({ .sizing = { fixed(28), fixed(28) }, .color = white, .rect_style = normal_rect_style }) {}
                ui_box({ .sizing = { fixed(28), fixed(28) }, .color = green, .rect_style = normal_rect_style }) {}
                ui_box({ .sizing = { fixed(28), fixed(28) }, .color = red,   .rect_style = normal_rect_style }) {}
            }
            // Bottom text
            ui_text(ui_context, glyph_cache, str("你好，世界"),
                &(TextConfig){ .font = font_zh,   .font_size = 11, .color = white, .line_height = 14 });
            ui_text(ui_context, glyph_cache, str("v1.0.0-beta"),
                &(TextConfig){ .font = font_mono, .font_size = 11, .color = green, .line_height = 14 });
        }

        // ─── Main Content Area ─────────────────────────────────────────────
        ui_box({
            .sizing = { fit_grow({}), fit_grow({}) },
            .color = grey,
            .rect_style = normal_rect_style,
            .padding = padding_big,
            .child_gap = child_gap_big,
            .direction = LAYOUT_TOP_TO_BOTTOM
        }) {
            // ── Top Header ──────────────────────────────────────
            ui_box({
                .sizing = { fit_grow({}), fit({}) },
                .child_gap = child_gap_medium,
                .direction = LAYOUT_LEFT_TO_RIGHT,
                .alignment = { .y = ALIGN_CENTER }
            }) {
                // Title block (min/max constraints)
                ui_box({
                    .sizing = { fit_grow({ .min = 200, .max = 340 }), fit({}) },
                    .color = black,
                    .rect_style = normal_rect_style,
                    .padding = padding_medium,
                    .child_gap = child_gap_smaller,
                    .direction = LAYOUT_TOP_TO_BOTTOM
                }) {
                    ui_text(ui_context, glyph_cache, str("系统监控台"),
                        &(TextConfig){ .font = font_zh,  .font_size = 16, .color = white, .line_height = 20 });
                    ui_text(ui_context, glyph_cache, str("Dashboard · Build 20240315"),
                        &(TextConfig){ .font = font_mono, .font_size = 11, .color = green, .line_height = 14 });
                }
                // Flexible blank space
                ui_box({ .sizing = { fit_grow({}), fixed(1) } }) {}
                // Three status badges
                ui_box({
                    .sizing = { fit({}), fit({}) },
                    .color = green,
                    .rect_style = normal_rect_style,
                    .padding = padding_small,
                    .alignment = { .x = ALIGN_CENTER, .y = ALIGN_CENTER }
                }) {
                    ui_text(ui_context, glyph_cache, str("● ONLINE"),
                        &(TextConfig){ .font = font_mono, .font_size = 11, .color = black, .line_height = 14 });
                }
                ui_box({
                    .sizing = { fit({}), fit({}) },
                    .color = red,
                    .rect_style = normal_rect_style,
                    .padding = padding_small,
                    .alignment = { .x = ALIGN_CENTER, .y = ALIGN_CENTER }
                }) {
                    ui_text(ui_context, glyph_cache, str("2 WARN"),
                        &(TextConfig){ .font = font_mono, .font_size = 11, .color = white, .line_height = 14 });
                }
                ui_box({
                    .sizing = { fit({}), fit({}) },
                    .color = grey,
                    .rect_style = normal_rect_style,
                    .padding = padding_small,
                    .alignment = { .x = ALIGN_CENTER, .y = ALIGN_CENTER }
                }) {
                    ui_text(ui_context, glyph_cache, str("IDLE"),
                        &(TextConfig){ .font = font_mono, .font_size = 11, .color = white, .line_height = 14 });
                }
            }

            // ── Card Row (4 equally spaced columns) ───────────────────────
            ui_box({
                .sizing = { fit_grow({}), fit_grow({ .max = 200 }) },
                .child_gap = child_gap_medium,
                .direction = LAYOUT_LEFT_TO_RIGHT
            }) {
                // Card 1: CPU — icon + multi-line text + progress bar
                ui_box({
                    .sizing = { fit_grow({}), fit_grow({}) },
                    .color = black,
                    .rect_style = full_rect_style,
                    .padding = padding_medium,
                    .child_gap = child_gap_small,
                    .direction = LAYOUT_TOP_TO_BOTTOM
                }) {
                    ui_box({
                        .sizing = { fit_grow({}), fit({}) },
                        .child_gap = child_gap_small,
                        .direction = LAYOUT_LEFT_TO_RIGHT,
                        .alignment = { .y = ALIGN_CENTER }
                    }) {
                        ui_box({ .sizing = { fixed(20), fixed(20) }, .color = green, .rect_style = normal_rect_style }) {}
                        ui_text(ui_context, glyph_cache, str("CPU"),
                            &(TextConfig){ .font = font_ui, .font_size = 14, .color = white, .line_height = 18 });
                    }
                    ui_text(ui_context, glyph_cache, str("72%"),
                        &(TextConfig){ .font = font_mono, .font_size = 22, .color = green, .line_height = 28 });
                    ui_text(ui_context, glyph_cache, str("4 cores · 3.2 GHz"),
                        &(TextConfig){ .font = font_mono, .font_size = 10, .color = grey, .line_height = 13 });
                    // Progress bar background + foreground (nested)
                    ui_box({
                        .sizing = { fit_grow({}), fixed(8) },
                        .color = grey,
                        .rect_style = normal_rect_style
                    }) {
                        ui_box({ .sizing = { fixed(100), fixed(8) }, .color = green, .rect_style = normal_rect_style }) {}
                    }
                }
                // Card 2: MEM
                ui_box({
                    .sizing = { fit_grow({}), fit_grow({}) },
                    .color = black,
                    .rect_style = full_rect_style,
                    .padding = padding_medium,
                    .child_gap = child_gap_small,
                    .direction = LAYOUT_TOP_TO_BOTTOM
                }) {
                    ui_box({
                        .sizing = { fit_grow({}), fit({}) },
                        .child_gap = child_gap_small,
                        .direction = LAYOUT_LEFT_TO_RIGHT,
                        .alignment = { .y = ALIGN_CENTER }
                    }) {
                        ui_box({ .sizing = { fixed(20), fixed(20) }, .color = blue, .rect_style = normal_rect_style }) {}
                        ui_text(ui_context, glyph_cache, str("MEM"),
                            &(TextConfig){ .font = font_ui, .font_size = 14, .color = white, .line_height = 18 });
                    }
                    ui_text(ui_context, glyph_cache, str("14.2G"),
                        &(TextConfig){ .font = font_mono, .font_size = 22, .color = blue, .line_height = 28 });
                    ui_text(ui_context, glyph_cache, str("/ 32G  DDR5"),
                        &(TextConfig){ .font = font_mono, .font_size = 10, .color = grey, .line_height = 13 });
                    ui_box({
                        .sizing = { fit_grow({}), fixed(8) },
                        .color = grey,
                        .rect_style = normal_rect_style
                    }) {
                        ui_box({ .sizing = { fixed(60), fixed(8) }, .color = blue, .rect_style = normal_rect_style }) {}
                    }
                }
                // Card 3: NET
                ui_box({
                    .sizing = { fit_grow({}), fit_grow({}) },
                    .color = black,
                    .rect_style = full_rect_style,
                    .padding = padding_medium,
                    .child_gap = child_gap_small,
                    .direction = LAYOUT_TOP_TO_BOTTOM
                }) {
                    ui_box({
                        .sizing = { fit_grow({}), fit({}) },
                        .child_gap = child_gap_small,
                        .direction = LAYOUT_LEFT_TO_RIGHT,
                        .alignment = { .y = ALIGN_CENTER }
                    }) {
                        ui_box({ .sizing = { fixed(20), fixed(20) }, .color = red, .rect_style = normal_rect_style }) {}
                        ui_text(ui_context, glyph_cache, str("NET"),
                            &(TextConfig){ .font = font_ui, .font_size = 14, .color = white, .line_height = 18 });
                    }
                    ui_text(ui_context, glyph_cache, str("↑ 1.2M"),
                        &(TextConfig){ .font = font_mono, .font_size = 13, .color = green, .line_height = 17 });
                    ui_text(ui_context, glyph_cache, str("↓ 8.7M"),
                        &(TextConfig){ .font = font_mono, .font_size = 13, .color = red, .line_height = 17 });
                    ui_text(ui_context, glyph_cache, str("eth0 · 1Gbps"),
                        &(TextConfig){ .font = font_mono, .font_size = 10, .color = grey, .line_height = 13 });
                }
                // Card 4: DISK (TOP_TO_BOTTOM with two fixed-height nested blocks)
                ui_box({
                    .sizing = { fit_grow({}), fit_grow({}) },
                    .color = black,
                    .rect_style = full_rect_style,
                    .padding = padding_medium,
                    .child_gap = child_gap_small,
                    .direction = LAYOUT_TOP_TO_BOTTOM
                }) {
                    ui_box({
                        .sizing = { fit_grow({}), fit({}) },
                        .child_gap = child_gap_small,
                        .direction = LAYOUT_LEFT_TO_RIGHT,
                        .alignment = { .y = ALIGN_CENTER }
                    }) {
                        ui_box({ .sizing = { fixed(20), fixed(20) }, .color = white, .rect_style = normal_rect_style }) {}
                        ui_text(ui_context, glyph_cache, str("DISK"),
                            &(TextConfig){ .font = font_ui, .font_size = 14, .color = white, .line_height = 18 });
                    }
                    ui_box({ .sizing = { fit_grow({}), fit_grow({}) } }) {}
                    ui_box({
                        .sizing = { fit_grow({}), fixed(28) },
                        .color = grey,
                        .rect_style = normal_rect_style,
                        .padding = { 4, 4, 4, 4 },
                        .alignment = { .x = ALIGN_END, .y = ALIGN_CENTER }
                    }) {
                        ui_box({ .sizing = { fixed(120), fit_grow({}) }, .color = red, .rect_style = normal_rect_style }) {}
                        ui_box({ .sizing = { fixed(60), fit_grow({}) },  .color = green, .rect_style = normal_rect_style }) {}
                    }
                    ui_text(ui_context, glyph_cache, str("480G / 1T  NVMe"),
                        &(TextConfig){ .font = font_mono, .font_size = 10, .color = grey, .line_height = 13 });
                }
            }

            // ── Middle: Log Panel + Side Info Column ─────────────────────
            ui_box({
                .sizing = { fit_grow({}), fit_grow({}) },
                .child_gap = child_gap_big,
                .direction = LAYOUT_LEFT_TO_RIGHT
            }) {
                // Log Panel
                ui_box({
                    .sizing = { fit_grow({}), fit_grow({}) },
                    .color = black,
                    .rect_style = full_rect_style,
                    .padding = padding_medium,
                    .child_gap = child_gap_smaller,
                    .direction = LAYOUT_TOP_TO_BOTTOM
                }) {
                    ui_text(ui_context, glyph_cache, str("事件日志"),
                        &(TextConfig){ .font = font_zh, .font_size = 13, .color = white, .line_height = 18 });
                    ui_box({ .sizing = { fit_grow({}), fixed(1) }, .color = grey }) {}
                    // 7 log entries
                    ui_box({
                        .sizing = { fit_grow({}), fit({}) },
                        .child_gap = child_gap_small,
                        .direction = LAYOUT_LEFT_TO_RIGHT,
                        .alignment = { .y = ALIGN_CENTER }
                    }) {
                        ui_box({ .sizing = { fixed(8), fixed(8) }, .color = green, .rect_style = normal_rect_style }) {}
                        ui_text(ui_context, glyph_cache, str("[INFO] Service started successfully"),
                            &(TextConfig){ .font = font_mono, .font_size = 11, .color = green, .line_height = 15 });
                    }
                    ui_box({
                        .sizing = { fit_grow({}), fit({}) },
                        .child_gap = child_gap_small,
                        .direction = LAYOUT_LEFT_TO_RIGHT,
                        .alignment = { .y = ALIGN_CENTER }
                    }) {
                        ui_box({ .sizing = { fixed(8), fixed(8) }, .color = red, .rect_style = normal_rect_style }) {}
                        ui_text(ui_context, glyph_cache, str("[WARN] Disk usage above 80%"),
                            &(TextConfig){ .font = font_mono, .font_size = 11, .color = red, .line_height = 15 });
                    }
                    ui_box({
                        .sizing = { fit_grow({}), fit({}) },
                        .child_gap = child_gap_small,
                        .direction = LAYOUT_LEFT_TO_RIGHT,
                        .alignment = { .y = ALIGN_CENTER }
                    }) {
                        ui_box({ .sizing = { fixed(8), fixed(8) }, .color = blue, .rect_style = normal_rect_style }) {}
                        ui_text(ui_context, glyph_cache, str("[INFO] Backup completed in 3.2s"),
                            &(TextConfig){ .font = font_mono, .font_size = 11, .color = white, .line_height = 15 });
                    }
                    ui_box({
                        .sizing = { fit_grow({}), fit({}) },
                        .child_gap = child_gap_small,
                        .direction = LAYOUT_LEFT_TO_RIGHT,
                        .alignment = { .y = ALIGN_CENTER }
                    }) {
                        ui_box({ .sizing = { fixed(8), fixed(8) }, .color = red, .rect_style = normal_rect_style }) {}
                        ui_text(ui_context, glyph_cache, str("[WARN] High network latency detected"),
                            &(TextConfig){ .font = font_mono, .font_size = 11, .color = red, .line_height = 15 });
                    }
                    ui_box({
                        .sizing = { fit_grow({}), fit({}) },
                        .child_gap = child_gap_small,
                        .direction = LAYOUT_LEFT_TO_RIGHT,
                        .alignment = { .y = ALIGN_CENTER }
                    }) {
                        ui_box({ .sizing = { fixed(8), fixed(8) }, .color = green, .rect_style = normal_rect_style }) {}
                        ui_text(ui_context, glyph_cache, str("[INFO] 缓存已刷新"),
                            &(TextConfig){ .font = font_zh, .font_size = 11, .color = white, .line_height = 15 });
                    }
                    ui_box({
                        .sizing = { fit_grow({}), fit({}) },
                        .child_gap = child_gap_small,
                        .direction = LAYOUT_LEFT_TO_RIGHT,
                        .alignment = { .y = ALIGN_CENTER }
                    }) {
                        ui_box({ .sizing = { fixed(8), fixed(8) }, .color = grey, .rect_style = normal_rect_style }) {}
                        ui_text(ui_context, glyph_cache, str("[DEBUG] heap_alloc: 0x7fffd3a0"),
                            &(TextConfig){ .font = font_mono, .font_size = 11, .color = grey, .line_height = 15 });
                    }
                    ui_box({
                        .sizing = { fit_grow({}), fit({}) },
                        .child_gap = child_gap_small,
                        .direction = LAYOUT_LEFT_TO_RIGHT,
                        .alignment = { .y = ALIGN_CENTER }
                    }) {
                        ui_box({ .sizing = { fixed(8), fixed(8) }, .color = green, .rect_style = normal_rect_style }) {}
                        ui_text(ui_context, glyph_cache, str("[INFO] Worker thread pool ready"),
                            &(TextConfig){ .font = font_mono, .font_size = 11, .color = green, .line_height = 15 });
                    }
                    // Flexible filler
                    ui_box({ .sizing = { fit_grow({}), fit_grow({}) } }) {}
                    // Bottom action bar
                    ui_box({
                        .sizing = { fit_grow({}), fit({}) },
                        .child_gap = child_gap_small,
                        .direction = LAYOUT_LEFT_TO_RIGHT,
                        .alignment = { .x = ALIGN_END, .y = ALIGN_CENTER }
                    }) {
                        ui_box({
                            .sizing = { fit({}), fit({}) },
                            .color = grey,
                            .rect_style = normal_rect_style,
                            .padding = padding_small,
                            .alignment = { .x = ALIGN_CENTER, .y = ALIGN_CENTER }
                        }) {
                            ui_text(ui_context, glyph_cache, str("清空"),
                                &(TextConfig){ .font = font_zh, .font_size = 12, .color = white, .line_height = 16 });
                        }
                        ui_box({
                            .sizing = { fit({}), fit({}) },
                            .color = blue,
                            .rect_style = normal_rect_style,
                            .padding = padding_small,
                            .alignment = { .x = ALIGN_CENTER, .y = ALIGN_CENTER }
                        }) {
                            ui_text(ui_context, glyph_cache, str("导出日志"),
                                &(TextConfig){ .font = font_zh, .font_size = 12, .color = white, .line_height = 16 });
                        }
                    }
                }

                // Right side info column
                ui_box({
                    .sizing = { fit({ .min = 160, .max = 220 }), fit_grow({}) },
                    .child_gap = child_gap_medium,
                    .direction = LAYOUT_TOP_TO_BOTTOM
                }) {
                    // Process list (fixed height)
                    ui_box({
                        .sizing = { fit_grow({}), fixed(180) },
                        .color = black,
                        .rect_style = full_rect_style,
                        .padding = padding_medium,
                        .child_gap = child_gap_smaller,
                        .direction = LAYOUT_TOP_TO_BOTTOM
                    }) {
                        ui_text(ui_context, glyph_cache, str("进程"),
                            &(TextConfig){ .font = font_zh, .font_size = 12, .color = white, .line_height = 16 });
                        ui_box({ .sizing = { fit_grow({}), fixed(1) }, .color = grey }) {}
                        ui_box({
                            .sizing = { fit_grow({}), fixed(24) },
                            .color = grey,
                            .rect_style = normal_rect_style,
                            .padding = { 4, 4, 4, 4 },
                            .direction = LAYOUT_LEFT_TO_RIGHT,
                            .alignment = { .y = ALIGN_CENTER }
                        }) {
                            ui_box({ .sizing = { fixed(6), fixed(6) }, .color = green }) {}
                            ui_box({ .sizing = { fit_grow({}), fixed(1) } }) {}
                            ui_text(ui_context, glyph_cache, str("nginx"),
                                &(TextConfig){ .font = font_mono, .font_size = 11, .color = white, .line_height = 14 });
                        }
                        ui_box({
                            .sizing = { fit_grow({}), fixed(24) },
                            .color = grey,
                            .rect_style = normal_rect_style,
                            .padding = { 4, 4, 4, 4 },
                            .direction = LAYOUT_LEFT_TO_RIGHT,
                            .alignment = { .y = ALIGN_CENTER }
                        }) {
                            ui_box({ .sizing = { fixed(6), fixed(6) }, .color = green }) {}
                            ui_box({ .sizing = { fit_grow({}), fixed(1) } }) {}
                            ui_text(ui_context, glyph_cache, str("postgres"),
                                &(TextConfig){ .font = font_mono, .font_size = 11, .color = white, .line_height = 14 });
                        }
                        ui_box({
                            .sizing = { fit_grow({}), fixed(24) },
                            .color = grey,
                            .rect_style = normal_rect_style,
                            .padding = { 4, 4, 4, 4 },
                            .direction = LAYOUT_LEFT_TO_RIGHT,
                            .alignment = { .y = ALIGN_CENTER }
                        }) {
                            ui_box({ .sizing = { fixed(6), fixed(6) }, .color = red }) {}
                            ui_box({ .sizing = { fit_grow({}), fixed(1) } }) {}
                            ui_text(ui_context, glyph_cache, str("redis"),
                                &(TextConfig){ .font = font_mono, .font_size = 11, .color = red, .line_height = 14 });
                        }
                        ui_box({
                            .sizing = { fit_grow({}), fixed(24) },
                            .color = grey,
                            .rect_style = normal_rect_style,
                            .padding = { 4, 4, 4, 4 },
                            .direction = LAYOUT_LEFT_TO_RIGHT,
                            .alignment = { .y = ALIGN_CENTER }
                        }) {
                            ui_box({ .sizing = { fixed(6), fixed(6) }, .color = green }) {}
                            ui_box({ .sizing = { fit_grow({}), fixed(1) } }) {}
                            ui_text(ui_context, glyph_cache, str("app_worker"),
                                &(TextConfig){ .font = font_mono, .font_size = 11, .color = white, .line_height = 14 });
                        }
                    }
                    // Timestamp block (grow)
                    ui_box({
                        .sizing = { fit_grow({}), fit_grow({}) },
                        .color = black,
                        .rect_style = full_rect_style,
                        .padding = padding_medium,
                        .child_gap = child_gap_smaller,
                        .direction = LAYOUT_TOP_TO_BOTTOM,
                        .alignment = { .x = ALIGN_CENTER }
                    }) {
                        ui_text(ui_context, glyph_cache, str("系统时间"),
                            &(TextConfig){ .font = font_zh,   .font_size = 11, .color = grey,  .line_height = 15 });
                        ui_text(ui_context, glyph_cache, str("2024-03-15"),
                            &(TextConfig){ .font = font_mono, .font_size = 13, .color = white, .line_height = 18 });
                        ui_text(ui_context, glyph_cache, str("14:23:07"),
                            &(TextConfig){ .font = font_mono, .font_size = 20, .color = green, .line_height = 26 });
                        ui_box({ .sizing = { fit_grow({}), fit_grow({}) } }) {}
                        ui_box({
                            .sizing = { fit_grow({}), fixed(4) },
                            .color = green,
                            .rect_style = normal_rect_style
                        }) {}
                        ui_box({
                            .sizing = { fit_grow({}), fixed(4) },
                            .color = blue,
                            .rect_style = normal_rect_style
                        }) {}
                    }
                }
            }

            // ── Bottom Toolbar ──────────────────────────────────────
            ui_box({
                .sizing = { fit_grow({}), fit({}) },
                .color = black,
                .rect_style = normal_rect_style,
                .padding = padding_small,
                .child_gap = child_gap_medium,
                .direction = LAYOUT_LEFT_TO_RIGHT,
                .alignment = { .x = ALIGN_END, .y = ALIGN_CENTER }
            }) {
                ui_box({
                    .sizing = { fixed(80), fixed(32) },
                    .color = grey,
                    .rect_style = normal_rect_style,
                    .padding = { 3, 3, 3, 3 },
                    .alignment = { .x = ALIGN_CENTER, .y = ALIGN_CENTER }
                }) {
                    ui_box({ .sizing = { fixed(14), fixed(14) }, .color = red }) {}
                }
                ui_box({
                    .sizing = { fixed(80), fixed(32) },
                    .color = grey,
                    .rect_style = normal_rect_style,
                    .padding = { 3, 3, 3, 3 },
                    .alignment = { .x = ALIGN_CENTER, .y = ALIGN_CENTER }
                }) {
                    ui_box({ .sizing = { fixed(14), fixed(14) }, .color = green }) {}
                }
                ui_box({
                    .sizing = { fit({}), fixed(32) },
                    .color = blue,
                    .rect_style = full_rect_style,
                    .padding = padding_small,
                    .alignment = { .x = ALIGN_CENTER, .y = ALIGN_CENTER }
                }) {
                    ui_text(ui_context, glyph_cache, str("刷新全部"),
                        &(TextConfig){ .font = font_zh, .font_size = 12, .color = white, .line_height = 16 });
                }
            }
        }
    }

    ///

    UIBox* root = ui_box_get_root();
    ui_calculate_layout(ui_context, glyph_cache, root);
    ui_generate_render_commands(ui_context, root);

    // Draw
    for (isize i = 0; i < ui_context->command_queue.count; i++)
    {
        UICommand* cmd = &ui_context->command_queue.items[i];
        switch (cmd->type)
        {
            case UI_COMMAND_RECT:
                renderer_draw_rect(glyph_cache, cmd->rect.rect, cmd->rect.color, cmd->rect.style);
                break;
            case UI_COMMAND_TEXT:
                renderer_draw_text(app_context->dwrite_factory, glyph_cache, cmd->text.content, cmd->text.position, cmd->text.color, ui_context->dpi, cmd->text.font, cmd->text.font_size);
                break;
            default:
                Assert(0);
        }
    }
    arena_pop_to(&ui_context->arena, 0);

    // Present
    f32 dpi_scale = (f32)ui_context->dpi / USER_DEFAULT_SCREEN_DPI;
    u32 physical_client_width = (u32)(ui_context->client_width * dpi_scale);
    u32 physical_client_height = (u32)(ui_context->client_height * dpi_scale);
    renderer_flush_and_present(physical_client_width, physical_client_height);

    TracyCZoneEnd(ctx);
}

static LRESULT CALLBACK window_procedure(const HWND window, const u32 message, const WPARAM wparam, const LPARAM lparam)
{
    // Read passing data from param
    AppContext* app_context = NULL;
    UIContext* ui_context = NULL;
    GlyphCache* glyph_cache = NULL;
    {
        if (message == WM_CREATE)
        {
            CREATESTRUCT* create = (CREATESTRUCT*)(lparam);
            app_context = (AppContext*)(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)app_context);
        }
        else
        {
            LONG_PTR ptr = GetWindowLongPtrW(window, GWLP_USERDATA);
            app_context = (AppContext*)ptr;
        }

        if (app_context)
        {
            ui_context = &app_context->ui;
            glyph_cache = &app_context->glyph_cache;
        }
    }

    // Handle message
    switch (message)
    {
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            BeginPaint(window, &ps);
            {
#if !defined(NDEBUG) || defined(TRACY_ENABLE)
                LARGE_INTEGER starting_time, ending_time, elapsed_microseconds;
                LARGE_INTEGER frequency;
                QueryPerformanceFrequency(&frequency);
                QueryPerformanceCounter(&starting_time);
#endif
                process_frame(app_context);
#if !defined(NDEBUG) || defined(TRACY_ENABLE)
                QueryPerformanceCounter(&ending_time);
                elapsed_microseconds.QuadPart = ending_time.QuadPart - starting_time.QuadPart;
                elapsed_microseconds.QuadPart *= 1000000;
                elapsed_microseconds.QuadPart /= frequency.QuadPart;

                // Set window title to show frame time
                swprintf(app_context->title, MAX_TITLE_LENGTH, L"Frame Time: %lld μs", elapsed_microseconds.QuadPart);
                SetWindowTextW(window, app_context->title);
#endif
            }
            EndPaint(window, &ps);
        } return 0;

        case WM_MOUSEMOVE:
        {
            InvalidateRect(window, NULL, False);
        } return 0;

        case WM_KEYDOWN:
        {
            if (wparam == VK_ESCAPE)
                DestroyWindow(window);
        } return 0;

        case WM_SIZE:
        {
            f32 dpi_scale = (f32)ui_context->dpi / USER_DEFAULT_SCREEN_DPI;
            u32 physical_client_width = LOWORD(lparam);
            u32 physical_client_height = HIWORD(lparam);

            ui_context->client_width = (u32)ceil(physical_client_width / dpi_scale);
            ui_context->client_height = (u32)ceil(physical_client_height / dpi_scale);
            if (ui_context->client_width > 0 && ui_context->client_height > 0)
            {
                ui_context->on_resize(physical_client_width, physical_client_height);
                // Force an immediate repaint of entire client area to ensure the updated content is rendered promptly
                InvalidateRect(window, NULL, False);
            }
        } return 0;

        case WM_DPICHANGED:
        {
            ui_context->dpi = GetDpiForWindow(window);
            glyph_cache_deinit(glyph_cache);
            glyph_cache_init(glyph_cache, GLYPHS_LENGTH);
            renderer_recreate_glyph_atlas_texture(&glyph_cache->atlas);

            // NOTE:
            //   After a DPI change, the first frame still uses the old glyphs, so the visual quality is poor;
            //   the second frame renders correctly. To prevent the glitch we could capture the currently‑visible
            //   glyphs, rasterize them into the new atlas, and update the texture. Because the effect is minor, we
            //   keep the existing behavior.
            process_frame(app_context); // Rasterize needed glyphs

            // Set new window
            RECT* const suggested_rect = (RECT*)lparam;
            SetWindowPos(window, NULL, suggested_rect->left, suggested_rect->top,
                         suggested_rect->right - suggested_rect->left, suggested_rect->bottom - suggested_rect->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        } return 0;

        case WM_DESTROY:
        {
            PostQuitMessage(0);
        } return 0;
    }

    return DefWindowProcW(window, message, wparam, lparam);
}

#if !defined(NDEBUG) || defined(TRACY_ENABLE)
i32 WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR lpCmdLine, i32 nShowCmd)
#else
#pragma comment (lib, "libvcruntime")
#pragma comment (lib, "ucrt")
i32 WinMainCRTStartup()
#endif
{
    // Tell the DWM not to perform any automatic DPI scaling (Windows 10, v1607)
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Init context
    AppContext app_context = {
        .title = L"App Title",
        .ui = {
            .arena = arena_new(MB(16)),
            .dpi = GetDpiForSystem(),
            .client_width = CLIENT_WIDTH,
            .client_height = CLIENT_HEIGHT,
            .on_resize = swapchain_resize,
            .get_text_width = renderer_get_text_width_for_dpi,
            .get_text_height = renderer_get_text_height_for_dpi,
            .command_queue = { 0 },
        },
        .dwrite_factory = NULL,
        .fonts = NULL,
        .glyph_cache = {
            .arena = arena_new(MB(32)),
            .glyphs = NULL,
            .atlas = {
                .w = GLYPH_ATLAS_WIDTH,
                .h = GLYPH_ATLAS_HEIGHT,
                .bitmap = NULL,
                .next_x = 0,
                .next_y = 0,
                .maxy = 0,
            }
        }
    };

    // Create window
    HWND window;
    {
        f32 dpi_scale = (f32)app_context.ui.dpi / USER_DEFAULT_SCREEN_DPI;
        u32 physical_client_width = (u32)(app_context.ui.client_width * dpi_scale);
        u32 physical_client_height = (u32)(app_context.ui.client_height * dpi_scale);

        // Set the client position to screen center
        i32 screen_width = GetSystemMetrics(SM_CXSCREEN);
        i32 screen_height = GetSystemMetrics(SM_CYSCREEN);
        i32 x = (screen_width - physical_client_width) / 2;
        i32 y = (screen_height - physical_client_height) / 2;

        // Give the client area rectangle, get back the entire window rectangle
        RECT rect = { x, y, x + physical_client_width, y + physical_client_height };
        DWORD window_style = WS_OVERLAPPEDWINDOW;
        AdjustWindowRectEx(&rect, window_style, 0, 0);

        // Register window class
        WNDCLASSW wc = {
            .lpfnWndProc = window_procedure,
            .hInstance = GetModuleHandleW(NULL),
            .lpszClassName = L"window class",
        };
        RegisterClassW(&wc);

        // Create window with user data
        window = CreateWindowExW(0, wc.lpszClassName, app_context.title, window_style, rect.left, rect.top,
                                 rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, wc.hInstance, &app_context);
    }

    // Initialize dwrite factory, font, glyph cache and renderer
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory, (void**)&app_context.dwrite_factory);
    font_register(&app_context.fonts[FONT_INDEX_UI], app_context.dwrite_factory, L"Segoe UI");
    font_register(&app_context.fonts[FONT_INDEX_ZH], app_context.dwrite_factory, L"Microsoft YaHei");
    font_register(&app_context.fonts[FONT_INDEX_MONO], app_context.dwrite_factory, L"Consolas");
    glyph_cache_init(&app_context.glyph_cache, GLYPHS_LENGTH);
    renderer_init(window, &app_context.glyph_cache.atlas);

    // Render first frame before showing window
    process_frame(&app_context); // Rasterize needed glyphs
    process_frame(&app_context);
    ShowWindow(window, SW_SHOWDEFAULT);

    // Run message loop
    MSG message;
    while (GetMessageW(&message, NULL, 0, 0))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    // Clean
    renderer_deinit();
    glyph_cache_deinit(&app_context.glyph_cache);
    font_unregister(&app_context.fonts[FONT_INDEX_UI]);
    font_unregister(&app_context.fonts[FONT_INDEX_ZH]);
    font_unregister(&app_context.fonts[FONT_INDEX_MONO]);
    IDWriteFactory3_Release(app_context.dwrite_factory);

    arena_release(&app_context.ui.arena);
    arena_release(&app_context.glyph_cache.arena);

    return 0;
}
