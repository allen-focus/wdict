#pragma once

#include "utils.h"

#include <string.h>  // for memcpy in dict_rd_u32

//
// Constants
//

#define DICT_MAGIC    0x44494354u /* "DICT" */
#define DICT_VERSION  1u

//
// PosKind — packed as u8 in the binary blob.
//

typedef enum {
    POS_NOUN = 0,
    POS_VERB,
    POS_ADJ,
    POS_ADV,
    POS_PREP,
    POS_CONJ,
    POS_INTERJ,
    POS_PRON,
    POS_ARTICLE,
    POS_PHRASE,
    POS_NUM,
    POS_UNKNOWN = 0xFF
} PosKind;

//
// On-disk binary structures (packed — no padding).
//

#pragma pack(push, 1)

typedef struct
{
    u32 magic;
    u32 version;
    u32 word_count;
    u32 words_off;    // → WordIndex[]
    u32 entdata_off;  // → EntryData
    u32 strpool_off;  // → StringPool
    u32 _reserved[2];
} DictFileHeader;

typedef struct
{
    u32 word_stroff;  // → strpool (null-terminated word string)
    u32 entdata_off;  // → entdata (EntryBlob start)
    u32 freq;         // 0xFFFFFFFF = null
} DictWordIndex;

#pragma pack(pop)

//
// Runtime zero-copy view of a loaded dictionary blob.
// All pointers point into the embedded binary — no malloc, no copies.
//

typedef struct
{
    const DictFileHeader *hdr;
    const DictWordIndex  *words;    // [hdr->word_count]
    const u8             *entdata;  // tightly-packed EntryBlob array
    const char           *strpool;  // null-terminated concatenated strings
} DictDB;

//
// API
//

// Open a dictionary from an in-memory blob (RC resource, mmap, etc.).
// Validates magic and version; returns a zero-filled DictDB on failure
// (caller checks db.hdr != NULL).
DictDB dict_open(const void *blob);

// Case-insensitive binary search over the alphabetically-sorted WordIndex.
// Returns word_idx (0 … hdr->word_count-1) or -1 on miss.
i32 dict_lookup(const DictDB *db, const char *word);

// Fuzzy search over all headwords, weighted by word frequency.
//   query       — user input (UTF-8)
//   out_indices — caller-provided buffer, filled with word_idx values
//   max_results — capacity of out_indices[]
//   scratch     — temporary arena (caller resets between calls)
// Returns number of matches written to out_indices (≤ max_results).
i32 dict_fuzzy_search(const DictDB *db, const char *query, i32 *out_indices, i32 max_results, Arena *scratch);

// Convert a strpool offset to a C string pointer.
#define DICT_STR(db, off) ((const char *)((db)->strpool + (off)))

//
// EntryBlob low-level readers (used by entry_view.c to parse entry data).
// All are static inline so the compiler can fold the tiny helpers.
//

static inline u8 dict_rd_u8(const u8 **p)
{
    return *(*p)++;
}

static inline u32 dict_rd_u32(const u8 **p)
{
    u32 v;
    memcpy(&v, *p, 4);
    *p += 4;
    return v;
}

// Skip over a brief_en / brief_zh array: u8 count + count × u32 offsets.
static inline void dict_skip_brief_array(const u8 **p)
{
    u8 count = dict_rd_u8(p);
    *p += (usize)count * 4;
}
