#include "cmd.h"

#include "thirdparty/tracy/public/tracy/TracyC.h"
#include "tracy_config.h" // IWYU pragma: keep

/*
 * Parse the first whitespace-delimited token from text.
 *   "window.create w=600 h=600"  → { "window.create",  "w=600 h=600" }
 *   "tab.new"                    → { "tab.new",        {0}           }
 */
typedef struct
{
    String token;
    String rest;
} ParseFirstTokenResult;

static ParseFirstTokenResult parse_first_token(String text)
{
    ParseFirstTokenResult r;
    isize end = 0;
    while (end < text.len && text.data[end] != ' ')
        end++;
    r.token = (String){ text.data, end };
    if (end < text.len)
        r.rest = (String){ text.data + end + 1, text.len - end - 1 };
    else
        r.rest = (String){ 0 };
    return r;
}

/*
 * Find  key=value  in text.  Returns the value slice (after '=', up to next
 * space or end).  Returns {0} if key not found.
 */
static String find_value(String text, String key)
{
    for (isize i = 0; i + key.len + 1 <= text.len; i++)
    {
        if ((i == 0 || text.data[i - 1] == ' ') && memcmp(text.data + i, key.data, (size_t)key.len) == 0 &&
            text.data[i + key.len] == '=')
        {
            isize val_start = i + key.len + 1;
            isize val_end = val_start;
            while (val_end < text.len && text.data[val_end] != ' ')
                val_end++;
            return (String){ text.data + val_start, val_end - val_start };
        }
    }
    return (String){ 0 };
}

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
// Text argument parsing
//

i32 cmd_parse_i32(String text, String key, i32 def)
{
    String val = find_value(text, key);
    if (val.len == 0)
        return def;
    i32 result = 0;
    i32 sign = 1;
    isize i = 0;
    if (val.data[0] == '-')
    {
        sign = -1;
        i = 1;
    }
    else if (val.data[0] == '+')
    {
        i = 1;
    }
    for (; i < val.len; i++)
    {
        if (val.data[i] < '0' || val.data[i] > '9')
            return def;
        result = result * 10 + (val.data[i] - '0');
    }
    return result * sign;
}

u32 cmd_parse_u32(String text, String key, u32 def)
{
    String val = find_value(text, key);
    if (val.len == 0)
        return def;
    u32 result = 0;
    for (isize i = 0; i < val.len; i++)
    {
        if (val.data[i] < '0' || val.data[i] > '9')
            return def;
        result = result * 10 + (u32)(val.data[i] - '0');
    }
    return result;
}

i32 cmd_parse_axis(String text, String key, i32 def)
{
    String val = find_value(text, key);
    if (val.len != 1)
        return def;
    if (val.data[0] == 'X')
        return 0;
    if (val.data[0] == 'Y')
        return 1;
    return def;
}

String cmd_parse_string(String text, String key, String def)
{
    String val = find_value(text, key);
    return val.len ? val : def;
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

    /* Save & clear before iterating — handlers may re-enter. */
    CmdQueueNode* list = q->first;
    q->first = NULL;
    q->last = NULL;
    q->count = 0;

    if (list)
        for (CmdQueueNode* n = list; n; n = n->next)
        {
            String cmd_id = parse_first_token(n->cmd_text).token;
            CmdDef* def = cmd_find(reg, cmd_id);
            if (!def || !def->execute)
                continue;
            def->execute(def->userdata, n->cmd_text);
        }

    TracyCZoneEnd(ctx_cmd);
    arena_pop_to(q->arena, 0);
}
