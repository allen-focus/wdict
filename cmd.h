#pragma once

#include "utils.h"

typedef void (*CmdFn)(void* userdata, void* payload, isize payload_size, String cmd_text);

//
// Binary payload (pointer cache for command handlers)
//

typedef struct Panel Panel;
typedef struct PanelTab PanelTab;

typedef struct
{
    void* ctx;
    Panel* panel;
    Panel* to_panel;
    PanelTab* tab;
} CmdPayload;

//
// cmd registry
//

typedef struct CmdDef CmdDef;
struct CmdDef
{
    String id; // stable machine-readable ID, e.g. "panel.split_h"
    String name; // human-readable display name
    String description; // tooltip / palette description
    CmdFn execute;
    void* userdata;
};

typedef struct
{
    CmdDef* defs;
    isize count;
    isize capacity;
} CmdRegistry;

void cmd_registry_init(CmdRegistry* reg, Arena* arena, isize capacity);
void cmd_register(CmdRegistry* reg, CmdDef def);
CmdDef* cmd_find(const CmdRegistry* reg, String id);

/*
 * Text argument parsing — no arena allocation.
 * Each scans `text` for `"key=..."` and returns the value after `=`,
 * stopping at the next space or end-of-string.  Returns `def` if the
 * key is not found or the value is malformed.
 */
i32 cmd_parse_i32(String text, String key, i32 def);
u32 cmd_parse_u32(String text, String key, u32 def);
i32 cmd_parse_axis(String text, String key, i32 def); // 0 ← "X", 1 ← "Y"

//
// cmd queue
//

typedef struct CmdQueueNode CmdQueueNode;
struct CmdQueueNode
{
    CmdQueueNode* next;
    String cmd_text; // canonical text; first token = cmd_id, rest = args
    isize payload_size;
    u8 payload[]; // optional binary pointer cache (CmdPayload or similar)
};

typedef struct
{
    Arena* arena;
    CmdQueueNode* first;
    CmdQueueNode* last;
    u64 count;
} CmdQueue;

void cmd_queue_init(CmdQueue* q, Arena* arena);

/*
 * Push a command.  cmd_id is extracted from the first whitespace-delimited
 * token of `text`.  `payload` is an optional binary pointer cache; pass
 * NULL / 0 if the handler works from text alone.
 */
CmdQueueNode* cmd_queue_push(CmdQueue* q, String text, const void* payload, isize payload_size);

void cmd_queue_execute_all(CmdQueue* q, const CmdRegistry* reg);
