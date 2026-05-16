#pragma once

#include "utils.h"

typedef void (*CmdFn)(void* userdata, void* context, void* payload, isize payload_size);

//
// cmd registry
//

typedef struct
{
    String id; // stable machine-readable ID, e.g. "panel.split_h"
    String name; // human-readable name, e.g. "Split Panel Horizontally"
    String description; // tooltip / palette description
    CmdFn execute;
    void* userdata;
} CmdDef;

typedef struct
{
    CmdDef* defs;
    isize count;
    isize capacity;
} CmdRegistry;

void cmd_registry_init(CmdRegistry* reg, Arena* arena, isize capacity);
void cmd_register(CmdRegistry* reg, CmdDef def);
CmdDef* cmd_find(const CmdRegistry* reg, String id);
void cmd_execute(const CmdRegistry* reg, String id, void* context, void* payload, isize payload_size);

//
// cmd queue
//

typedef struct CmdQueueNode CmdQueueNode;
struct CmdQueueNode
{
    CmdQueueNode* next;
    String cmd_id;
    isize payload_size;
    u8 payload[];
};

typedef struct
{
    Arena* arena;
    CmdQueueNode* first;
    CmdQueueNode* last;
    u64 count;
} CmdQueue;

void cmd_queue_init(CmdQueue* q, Arena* arena);
CmdQueueNode* cmd_queue_push(CmdQueue* q, String cmd_id, const void* payload, isize payload_size);
void cmd_queue_execute_all(CmdQueue* q, const CmdRegistry* reg, void* context);
