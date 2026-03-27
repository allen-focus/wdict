#include "pch.h"  // IWYU pragma: keep
#include "utils.h"
#include "LRU.h"

//
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
//
void lru_cache_create(Arena* arena, LRUCache* lru_cache, isize hash_chain_head_capacity, isize entry_capacity,
                      isize key_size, isize value_size, hash_fn hash, is_same_fn is_same)
{
    Assert(hash_chain_head_capacity > 0);
    Assert(entry_capacity > 0);

    memset(lru_cache, 0, sizeof(*lru_cache));
    lru_cache->hash_chain_heads = arena_push(arena, sizeof(isize), _Alignof(isize), hash_chain_head_capacity);
    lru_cache->entries = arena_push(arena, sizeof(Entry), _Alignof(Entry), entry_capacity);

    isize align = 16; // Hard-code align
    lru_cache->keys_buf   = arena_push(arena, key_size,   align, entry_capacity);
    lru_cache->values_buf = arena_push(arena, value_size, align, entry_capacity);

    // NOTE: Before the entry list becomes full, `sentinel->next_with_same_hash` indicates the index of the next free
    // entry. After the entry list is full, `sentinel->next_with_same_hash` is always 0. In this state, consecutive LRU
    // pops are not allowed. A pop must be immediately followed by an insertion to keep the cache at full
    Entry* sentinel = &lru_cache->entries[0];
    sentinel->next_with_same_hash = 1;

    lru_cache->hash_chain_head_capacity = hash_chain_head_capacity;
    lru_cache->entry_capacity = entry_capacity;
    lru_cache->key_size = key_size;
    lru_cache->value_size = value_size;

    lru_cache->hash = hash;
    lru_cache->is_same = is_same;
}

void lru_cache_destroy(LRUCache* lru_cache)
{
    memset(lru_cache, 0, sizeof(LRUCache));
}

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
//
static u32 lru_cache_pop_lru_entry(LRUCache* lru_cache)
{
    Entry* sentinel = &lru_cache->entries[0];
    Assert(sentinel->next_with_same_hash == 0);

    // Get the LRU entry (A LRU entry is the tail entry of the entry linked list)
    u32 lru_entry_index = sentinel->lru_prev;
    Entry* lru_entry = &lru_cache->entries[lru_entry_index];

    // Unlink the current LRU entry from the hash chain by updating its predecessor's `next_with_same_hash`
    u32* prev_hash_chain_entry_index = &lru_cache->hash_chain_heads[lru_entry->hash_chain_head_index];
    while (*prev_hash_chain_entry_index != lru_entry_index)
    {
        Entry* entry = &lru_cache->entries[*prev_hash_chain_entry_index];
        prev_hash_chain_entry_index = &entry->next_with_same_hash;
    }
    *prev_hash_chain_entry_index = lru_entry->next_with_same_hash;

    // Update the new LRU entry
    u32 new_lru_entry_index = lru_entry->lru_prev;
    Entry* new_lru_entry = &lru_cache->entries[new_lru_entry_index];
    new_lru_entry->lru_next = 0;

    // Update sentinel
    sentinel->lru_prev = new_lru_entry_index;

    return lru_entry_index;
}

// Situation 1 - Found existing entry N
// ====================================
//                                                                 |
// Before:                                                         |  After:
//                                                                 |
//    +-------------------------------------------------------+    |     +-------------------------------------------------------+
//    |                                                       |    |     |                                                       v
//    v         head                             head         V    |     v         head                                        head
// +-----+      +---+      +---+      +---+      +---+      +---+  |  +-----+      +---+      +---+      +---+      +---+      +---+
// | Sen | <--> | X | <--> | Y | <--> | Z | <--> | M | ---> | N |  |  | Sen | <--> | N | <--> | X | <--> | Y | <--> | Z | <--> | M |
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
// | Sen | <--> | X | <--> | Y | <--> | Z | <--> | M | ---> | N |  |  | Sen | <--> | O | <--> | X | <--> | Y | <--> | Z | <--> | M | ---> | N |
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
//
u32 lru_cache_find_or_insert(LRUCache* lru_cache, void* key, void* value)
{
    Entry* sentinel = &lru_cache->entries[0];

    u32 hash_value = lru_cache->hash(key, lru_cache->key_size);
    u32 hash_chain_head_index = hash_value & (lru_cache->hash_chain_head_capacity - 1);
    u32* hash_chain_head = &lru_cache->hash_chain_heads[hash_chain_head_index];

    Entry* entry = NULL;
    u32 entry_index = *hash_chain_head;

    // Check whether the entry exsits
    b32 existing = False;
    while (entry_index)
    {
        entry = &lru_cache->entries[entry_index];
        void* entry_key = (byte*)lru_cache->keys_buf + entry_index * lru_cache->key_size;
        if (lru_cache->is_same(entry_key, key, lru_cache->key_size))
        {
            existing = True;
            break;
        }
        entry_index = entry->next_with_same_hash;
    }

    if (existing)
    {
        // Found an existing entry, deattach from the entry linked list as a prepare
        Entry* prev_entry = &lru_cache->entries[entry->lru_prev];
        Entry* next_entry = &lru_cache->entries[entry->lru_next];
        prev_entry->lru_next = entry->lru_next;
        next_entry->lru_prev = entry->lru_prev;

        lru_cache->stats.hit_count++;
    }
    else
    {
        // Not found existing entry matched key, pop one
        if (sentinel->next_with_same_hash == 0)
        {
            // NOTE: `sentinel->next_with_same_hash` acts as a flag indicating whether the entry linked list is full.
            // As we pop an entry and quickly insert a new one, we must reset this flag to 0.
            sentinel->next_with_same_hash = 0;
            entry_index = lru_cache_pop_lru_entry(lru_cache);
            entry = &lru_cache->entries[entry_index];
        }
        else
        {
            entry_index = sentinel->next_with_same_hash;
            entry = &lru_cache->entries[entry_index];

            sentinel->next_with_same_hash++;
            if (sentinel->next_with_same_hash == lru_cache->entry_capacity)
                sentinel->next_with_same_hash = 0;
        }

        // Assign value
        void* entry_key = (byte*)lru_cache->keys_buf + entry_index * lru_cache->key_size;
        void* entry_value = (byte*)lru_cache->values_buf + entry_index * lru_cache->value_size;
        memcpy(entry_key, key, lru_cache->key_size);
        memcpy(entry_value, value, lru_cache->value_size);
        entry->hash_chain_head_index = hash_chain_head_index;

        // Insert the new entry at the head of the hash chain
        entry->next_with_same_hash = *hash_chain_head;
        *hash_chain_head = entry_index;

        lru_cache->stats.miss_count++;
    }

    // Insert the entry at the front of the entry linked list
    entry->lru_prev = 0;
    entry->lru_next = sentinel->lru_next;
    lru_cache->entries[sentinel->lru_next].lru_prev = entry_index;
    sentinel->lru_next = entry_index;

    return entry_index;
}
