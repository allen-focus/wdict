#pragma once

#include "utils.h"

///

#define HASH_CHAIN_HEAD_CAPACITY 4
#define ENTRY_CAPACITY 8

///

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
    isize hash_chain_head_capacity; // must be power of two
    isize entry_capacity; // must be power of two
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

void lru_cache_create(Arena* arena, LRUCache* lru_cache, isize hash_chain_head_capacity, isize entry_capacity,
                      isize key_size, isize value_size, hash_fn hash, is_same_fn is_same);
void lru_cache_destroy(LRUCache* lru_cache);
u32 lru_cache_find_or_insert(LRUCache* lru_cache, void* key, void* value);
