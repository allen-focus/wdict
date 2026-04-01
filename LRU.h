#pragma once

#include "utils.h"

///

typedef enum {
    LRU_SIGNAL_TOEVICT,
    LRU_SIGNAL_TOINSERT,
    LRU_SIGNAL_FOUND,
} LRUSignal;

typedef struct Entry {
    u32 hash_chain_head_index; // 4 bytes
    u32 next_with_same_hash;   // 4 bytes
    u32 lru_prev;              // 4 bytes
    u32 lru_next;              // 4 bytes
} Entry; // 16 bytes (L1 cache)

typedef struct {
    isize hit_count;
    isize miss_count;
} LRUCacheStats;

typedef u32 (*hash_fn)(const void* data, isize size);
typedef b32 (*is_same_fn)(const void* a, const void* b, isize size);

typedef struct {
    LRUCacheStats stats;
    isize hash_chain_head_capacity; // must be a power of two
    isize entry_capacity; // must be a power of two
    u32* hash_chain_heads; // for quickly finding the entry using the hash index
    Entry* entries;

    // NOTE: Store keys and values in contiguous buffers and should access them by computing offsets:
    //     entry_key   = (byte*)lru_cache->keys_buf   + entry_index * key_size;
    //     entry_value = (byte*)lru_cache->values_buf + entry_index * value_size;
    // This keep the `Entry` size 16 bytes (L1 cache), improving cache locality and performance.
    void* keys_buf;
    void* values_buf;
    isize key_size;
    isize value_size;

    hash_fn hash;
    is_same_fn is_same;
} LRUCache;

///

// NOTE: `hash_chain_head_capacity` must be a power of two
LRUCache lru_cache_create(Arena* arena, isize hash_chain_head_capacity, isize entry_capacity, isize key_size,
                          isize value_size, hash_fn hash, is_same_fn is_same);
void lru_cache_destroy(LRUCache* lru_cache);

// NOTE: User need to assign entry value using returned entry index
void lru_cache_remove_entry(LRUCache* lru_cache, u32 entry_index);
u32 lru_cache_pop_lru_entry(LRUCache* lru_cache);
u32 lru_cache_find(const LRUCache* lru_cache, const void* key, b32* found);
u32 lru_cache_find_or_evict(LRUCache* lru_cache, const void* key, LRUSignal* signal);
