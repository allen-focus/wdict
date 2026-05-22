#pragma once

#include "utils.h"

#define CMD_STR_MAX_LENGTH 256

typedef void (*CmdFn)(void* userdata, String cmd_text);

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
String cmd_parse_string(String text, String key, String def);

//
// cmd queue
//

typedef struct CmdQueueNode CmdQueueNode;
struct CmdQueueNode
{
    CmdQueueNode* next;
    String cmd_text; // canonical text; first token = cmd_id, rest = args
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
 * Push a command.  The text string is cloned into the queue's arena,
 * so stack-allocated snprintf buffers are safe.
 */
CmdQueueNode* cmd_queue_push(CmdQueue* q, String text);

void cmd_queue_execute_all(CmdQueue* q, const CmdRegistry* reg);
