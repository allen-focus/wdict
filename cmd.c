#include "cmd.h"
#include <string.h>

#include "thirdparty/tracy/public/tracy/TracyC.h"
#include "tracy_config.h" // IWYU pragma: keep

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

//
// Queue
//

void cmd_queue_init(CmdQueue* q, Arena* arena)
{
    memset(q, 0, sizeof(*q));
    q->arena = arena;
}

CmdQueueNode* cmd_queue_push(CmdQueue* q, String text)
{
    CmdQueueNode* n = (CmdQueueNode*)arena_push(q->arena, sizeof(CmdQueueNode), _Alignof(CmdQueueNode), 1);
    n->cmd_text = str_clone(q->arena, text);

    if (!q->last)
        q->first = n;
    else
        q->last->next = n;
    q->last = n;
    q->count++;
    return n;
}

void cmd_queue_execute_all(CmdQueue* q, const CmdRegistry* reg)
{
    TracyCZoneNC(ctx_cmd, "CmdExec", TracyColor_Cmd, TRACY_SUBSYSTEMS & TracySys_Cmd);

    isize arena_pos = q->arena->pos;

    CmdQueueNode* list = q->first;
    q->first = NULL;
    q->last = NULL;
    q->count = 0;

    for (CmdQueueNode* n = list; n; n = n->next)
    {
        /* Extract command ID: first whitespace-delimited token */
        isize end = 0;
        while (end < n->cmd_text.len && n->cmd_text.data[end] != ' ')
            end++;
        String cmd_id = { n->cmd_text.data, end };

        CmdDef* def = cmd_find(reg, cmd_id);
        if (def && def->execute)
            def->execute(def->userdata, n->cmd_text);
    }

    TracyCZoneEnd(ctx_cmd);
    arena_pop_to(q->arena, arena_pos);
}
