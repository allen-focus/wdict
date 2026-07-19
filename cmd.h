#pragma once

#include "utils.h"

typedef void (*CmdFn)(void* userdata, String cmd_text);

//
// cmd registry
//

typedef struct CmdDef CmdDef;
struct CmdDef
{
    String id;   // stable machine-readable ID, e.g. "palette.close"
    String name; // human-readable display name
    String description;
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

//
// cmd queue
//

typedef struct CmdQueueNode CmdQueueNode;
struct CmdQueueNode
{
    CmdQueueNode* next;
    String cmd_text; // first token = cmd_id, rest = args (if any)
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
 * Push a command.  The text is cloned into the queue's arena,
 * so stack-allocated buffers are safe.
 */
CmdQueueNode* cmd_queue_push(CmdQueue* q, String text);

void cmd_queue_execute_all(CmdQueue* q, const CmdRegistry* reg);
