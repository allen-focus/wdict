#include "cmd.h"

#include <string.h>

void cmd_registry_init(CmdRegistry* reg, Arena* arena, isize capacity)
{
    reg->defs = (CmdDef*)arena_push(arena, sizeof(CmdDef) * capacity, _Alignof(CmdDef), 1);
    reg->count = 0;
    reg->capacity = capacity;
}

void cmd_register(CmdRegistry* reg, CmdDef def)
{
    Assert(reg->count < reg->capacity);
    reg->defs[reg->count++] = def;
}

CmdDef* cmd_find(const CmdRegistry* reg, String id)
{
    for (isize i = 0; i < reg->count; i++)
        if (str_compare(reg->defs[i].id, id))
            return &reg->defs[i];
    return NULL;
}

void cmd_execute(const CmdRegistry* reg, String id, void* context, void* payload, isize payload_size)
{
    CmdDef* def = cmd_find(reg, id);
    if (def && def->execute)
        def->execute(def->userdata, context, payload, payload_size);
}

void cmd_queue_init(CmdQueue* q, Arena* arena)
{
    memset(q, 0, sizeof(*q));
    q->arena = arena;
}

CmdQueueNode* cmd_queue_push(CmdQueue* q, String cmd_id, const void* payload, isize payload_size)
{
    isize node_size = sizeof(CmdQueueNode) + payload_size;
    CmdQueueNode* n = (CmdQueueNode*)arena_push(q->arena, node_size, _Alignof(CmdQueueNode), 1);
    n->cmd_id = cmd_id;
    n->payload_size = payload_size;
    if (payload && payload_size > 0)
        memcpy(n->payload, payload, (size_t)payload_size);

    if (!q->last)
        q->first = n;
    else
        q->last->next = n;
    q->last = n;
    q->count++;
    return n;
}

void cmd_queue_execute_all(CmdQueue* q, const CmdRegistry* reg, void* context)
{
    /* Save and clear the list before iterating — some command handlers
       may re-enter process_frame → cmd_queue_execute_all, and the inner
       call must see an empty queue. */
    CmdQueueNode* list = q->first;
    q->first = NULL;
    q->last = NULL;
    q->count = 0;

    if (!list)
        return;

    for (CmdQueueNode* n = list; n; n = n->next)
        cmd_execute(reg, n->cmd_id, context, n->payload, n->payload_size);

    arena_pop_to(q->arena, 0);
}
