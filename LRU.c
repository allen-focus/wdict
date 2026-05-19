#include "LRU.h"
#include "utils.h"
#include <string.h>

#include "thirdparty/tracy/public/tracy/TracyC.h"
#include "tracy_config.h" // IWYU pragma: keep

// clang-format off
//                       +------------------------------------------------------------------------------------------+
//                       |                                                                                          |
//                       v           hash chain head                                       hash chain head          v
//                  +----------+       +---------+       +---------+       +---------+       +---------+       +---------+
// LRU double list: | Sentinel | <---> | Entry X | <---> | Entry Y | <---> | Entry Z | <---> | Entry M | <---> | Entry N |
//                  +----------+       +---------+       +---------+       +---------+       +---------+       +---------+
//                       ^                  |                                                     |            (LRU entry)
//                       |                  | next_with_same_hash                                 | next_with_same_hash
//                       |                  v                                                     v
//                       |             +---------+                                           +---------+
//                       |             | Entry N |                                           | Entry Y |
//                       |             +---------+                                           +---------+
//                       |                  | next_with_same_hash                                 |
//                       |                  v                                                     |
//                       |             +---------+                                                |
//                       |             | Entry Z |                                                |
//                       |             +---------+                                                |
//                       |                  | next_with_same_hash                                 |
//                       +------------------+-----------------------------------------------------+
// clang-format on
LRUCache lru_cache_create(Arena* arena, isize hash_chain_head_capacity, isize entry_capacity, isize key_size,
                          isize value_size, hash_fn hash, is_same_fn is_same)
{
    Assert(hash_chain_head_capacity > 0);
    Assert(entry_capacity > 0);
    LRUCache lru_cache = { 0 };

    lru_cache.hash_chain_heads = arena_push(arena, sizeof(u32), _Alignof(u32), hash_chain_head_capacity);
    lru_cache.hash_chain_head_capacity = hash_chain_head_capacity;
    lru_cache.entries = arena_push(arena, sizeof(Entry), _Alignof(Entry), entry_capacity);
    lru_cache.entry_capacity = entry_capacity;

    isize align = 16; // Hard-code align
    lru_cache.keys_buf = arena_push(arena, key_size, align, entry_capacity);
    lru_cache.values_buf = arena_push(arena, value_size, align, entry_capacity);
    lru_cache.key_size = key_size;
    lru_cache.value_size = value_size;

    lru_cache.hash = hash;
    lru_cache.is_same = is_same;

    // NOTE: The first entry is "sentinel". Any entry has same hash with sentinel is considered to be an free entry
    for (u32 i = 0; i < lru_cache.entry_capacity; i++)
        lru_cache.entries[i].next_with_same_hash = (i + 1 < entry_capacity) ? (i + 1) : 0;

    return lru_cache;
}

void lru_cache_destroy(LRUCache* lru_cache)
{
    memset(lru_cache, 0, sizeof(LRUCache));
}

// clang-format off
// Before popped:                                             |  After popped:
//                                                            |
//    +--------------------------------------------------+    |     +----------------------------------------+
//    |                                                  |    |     |                                        V
//    v        head                          head        V    |     v        head                          head
// +-----+     +---+     +---+     +---+     +---+     +---+  |  +-----+     +---+     +---+     +---+     +---+   +---+
// | Sen | <-> | X | <-> | Y | <-> | Z | <-> | M | <-> | N |  |  | Sen | <-> | X | <-> | Y | <-> | Z | <-> | M |   | N |
// +-----+     +---+     +---+     +---+     +---+     +---+  |  +-----+     +---+     +---+     +---+     +---+   +---+
//    ^          |                             |              |     ^          |                             |     (free)
//    |          v                             v              |     |          v                             v
//    |        +---+                         +---+            |     |        +---+                         +---+
//    |        | N |                         | Y |            |     |        | Z |                         | Y |
//    |        +---+                         +---+            |     |        +---+                         +---+
//    |          |                             |              |     |          |                             |
//    |          v                             |              |     +----------+-----------------------------+
//    |        +---+                           |              |
//    |        | Z |                           |              |
//    |        +---+                           |              |
//    |          |                             |              |
//    +----------+-----------------------------+              |
// clang-format on
void lru_cache_remove_entry(LRUCache* lru_cache, u32 entry_index)
{
    Entry* sentinel = &lru_cache->entries[0];
    Entry* selected_entry = &lru_cache->entries[entry_index];

    /* Unlink the selected entry from the hash chain by updating its predecessor's `next_with_same_hash` */
    u32* prev_hash_chain_entry_index = &lru_cache->hash_chain_heads[selected_entry->hash_chain_head_index];
    while (*prev_hash_chain_entry_index != entry_index)
    {
        Entry* entry = &lru_cache->entries[*prev_hash_chain_entry_index];
        prev_hash_chain_entry_index = &entry->next_with_same_hash;
    }
    *prev_hash_chain_entry_index = selected_entry->next_with_same_hash;

    /* Update near entries of the entry linked list */
    lru_cache->entries[selected_entry->lru_prev].lru_next = selected_entry->lru_next;
    lru_cache->entries[selected_entry->lru_next].lru_prev = selected_entry->lru_prev;
    selected_entry->lru_prev = 0;
    selected_entry->lru_next = 0;

    /* Update free entry hash chain */
    u32 next_free_entry_index = sentinel->next_with_same_hash;
    sentinel->next_with_same_hash = entry_index;
    selected_entry->next_with_same_hash = next_free_entry_index;
    selected_entry->hash_chain_head_index = 0;
}

u32 lru_cache_pop_lru_entry(LRUCache* lru_cache)
{
    /* LRU entry is the tail entry of the entry linked list */
    Entry* sentinel = &lru_cache->entries[0];
    u32 lru_entry_index = sentinel->lru_prev;
    Assert(lru_entry_index);
    lru_cache_remove_entry(lru_cache, lru_entry_index);
    return lru_entry_index;
}

// clang-format off
// Situation 1 - Found existing entry N
// ====================================
//                                                                 |
// Before:                                                         |  After:
//                                                                 |
//    +-------------------------------------------------------+    |     +-------------------------------------------------------+
//    |                                                       |    |     |                                                       v
//    v         head                             head         V    |     v         head                                        head
// +-----+      +---+      +---+      +---+      +---+      +---+  |  +-----+      +---+      +---+      +---+      +---+      +---+
// | Sen | <--> | X | <--> | Y | <--> | Z | <--> | M | <--> | N |  |  | Sen | <--> | N | <--> | X | <--> | Y | <--> | Z | <--> | M |
// +-----+      +---+      +---+      +---+      +---+      +---+  |  +-----+      +---+      +---+      +---+      +---+      +---+
//    ^           |                                |               |     ^           |                                           |
//    |           v                                v               |     |           v                                           v
//    |         +---+                            +---+             |     |         +---+                                       +---+
//    |         | N |                            | Y |             |     |         | X |                                       | Y |
//    |         +---+                            +---+             |     |         +---+                                       +---+
//    |           |                                |               |     |           |                                           |
//    |           v                                |               |     |           V                                           |
//    |         +---+                              |               |     |         +---+                                         |
//    |         | Z |                              |               |     |         | Z |                                         |
//    |         +---+                              |               |     |         +---+                                         |
//    |           |                                |               |     |           |                                           |
//    +-----------+--------------------------------+               |     +-----------+-------------------------------------------+
//
//
// Situation 2 - Insert a new entry O, and O has same hash with Y
// ==============================================================
//                                                                 |
// Before:                                                         |  After:
//                                                                 |
//    +-------------------------------------------------------+    |     +------------------------------------------------------------------+
//    |                                                       |    |     |                                                                  |
//    v         head                             head         V    |     v         head       head                                          V
// +-----+      +---+      +---+      +---+      +---+      +---+  |  +-----+      +---+      +---+      +---+      +---+      +---+      +---+
// | Sen | <--> | X | <--> | Y | <--> | Z | <--> | M | <--> | N |  |  | Sen | <--> | O | <--> | X | <--> | Y | <--> | Z | <--> | M | <--> | N |
// +-----+      +---+      +---+      +---+      +---+      +---+  |  +-----+      +---+      +---+      +---+      +---+      +---+      +---+
//    ^           |                                |               |     ^           |          |
//    |           v                                v               |     |           v          v
//    |         +---+                            +---+             |     |         +---+      +---+
//    |         | N |                            | Y |             |     |         | M |      | N |
//    |         +---+                            +---+             |     |         +---+      +---+
//    |           |                                |               |     |           |          |
//    |           v                                |               |     |           v          v
//    |         +---+                              |               |     |         +---+      +---+
//    |         | Z |                              |               |     |         | Y |      | Z |
//    |         +---+                              |               |     |         +---+      +---+
//    |           |                                |               |     |           |          |
//    +-----------+--------------------------------+               |     +-----------+----------+
// clang-format on
LRUCacheFindResult lru_cache_find(const LRUCache* lru_cache, const void* key)
{
    LRUCacheFindResult result = { 0 };

    u32 hash_chain_head_index = lru_cache->hash(key, lru_cache->key_size) & (lru_cache->hash_chain_head_capacity - 1);
    u32 entry_index = lru_cache->hash_chain_heads[hash_chain_head_index];

    result.found = False;
    while (entry_index)
    {
        void* entry_key = (byte*)lru_cache->keys_buf + entry_index * lru_cache->key_size;
        if (lru_cache->is_same(entry_key, key, lru_cache->key_size))
        {
            result.found = True;
            break;
        }
        entry_index = lru_cache->entries[entry_index].next_with_same_hash;
    }
    result.index = entry_index;
    return result;
}

LRUCacheFindOrEvictResult lru_cache_find_or_evict(LRUCache* lru_cache, const void* key)
{
    TracyCZoneNC(ctx_lru, "LRUFindOrEvict", TracyColor_Cache, TRACY_SUBSYSTEMS & TracySys_Cache);
    LRUCacheFindOrEvictResult result = { 0 };
    Entry* sentinel = &lru_cache->entries[0];

    u32 hash_chain_head_index = lru_cache->hash(key, lru_cache->key_size) & (lru_cache->hash_chain_head_capacity - 1);
    u32* hash_chain_head = &lru_cache->hash_chain_heads[hash_chain_head_index];

    u32 entry_index = *hash_chain_head;
    Entry* entry = NULL;

    /* Check whether the entry exists */
    result.signal = LRU_SIGNAL_TOINSERT;
    while (entry_index)
    {
        entry = &lru_cache->entries[entry_index];
        void* entry_key = (byte*)lru_cache->keys_buf + entry_index * lru_cache->key_size;
        if (lru_cache->is_same(entry_key, key, lru_cache->key_size))
        {
            result.signal = LRU_SIGNAL_FOUND;
            break;
        }
        entry_index = entry->next_with_same_hash;
    }

    if (result.signal == LRU_SIGNAL_FOUND)
    {
        /* Found an existing entry, deattach from the entry linked list as a prepare */
        Entry* prev_entry = &lru_cache->entries[entry->lru_prev];
        Entry* next_entry = &lru_cache->entries[entry->lru_next];
        prev_entry->lru_next = entry->lru_next;
        next_entry->lru_prev = entry->lru_prev;

        lru_cache->stats.hit_count++;
    }
    else
    {
        /* If the cache is full, pop one; else insert one to next empty entry slot */
        if (sentinel->next_with_same_hash == 0)
        {
            result.signal = LRU_SIGNAL_TOEVICT;
            entry_index = lru_cache_pop_lru_entry(lru_cache);
        }
        else
        {
            result.signal = LRU_SIGNAL_TOINSERT;
            entry_index = sentinel->next_with_same_hash;
        }
        entry = &lru_cache->entries[entry_index];
        sentinel->next_with_same_hash = entry->next_with_same_hash;

        /* Assign value */
        void* entry_key = (byte*)lru_cache->keys_buf + entry_index * lru_cache->key_size;
        memcpy(entry_key, key, lru_cache->key_size);
        entry->hash_chain_head_index = hash_chain_head_index;

        /* Insert the new entry at the head of the hash chain */
        entry->next_with_same_hash = *hash_chain_head;
        *hash_chain_head = entry_index;

        lru_cache->stats.miss_count++;
    }
    result.index = entry_index;

    /* Insert the entry at the front of the entry linked list */
    entry->lru_prev = 0;
    entry->lru_next = sentinel->lru_next;
    lru_cache->entries[sentinel->lru_next].lru_prev = entry_index;
    sentinel->lru_next = entry_index;

    TracyCZoneEnd(ctx_lru);
    return result;
}
