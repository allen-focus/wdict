#pragma once

#include "utils.h"
#include <string.h>

//
// Constants
//

#define DICT_MAGIC           0x44494354u /* "DICT" */
#define DICT_VERSION         4u /* v4: flat 5-field format (phonetic, definition, translation, exchange) */

//
// On-disk binary structures
//

typedef struct
{
    u32 magic;
    u32 version;
    u32 word_count;
    u32 words_off; // → WordIndex[]
    u32 entdata_off; // → EntryData
    u32 strpool_off; // → StringPool
    u32 variant_off; // reserved (v4 ships no VariantIndex; always 0)
    u32 variant_count; // reserved (always 0)
} DictFileHeader;

typedef struct
{
    u32 word_stroff; // → strpool (null-terminated word string)
    u32 entdata_off; // → entdata (EntryBlob start)
    u32 freq; // 0xFFFFFFFF = null
} DictWordIndex;

//
// Runtime zero-copy view of a loaded dictionary blob.
// All pointers point into the embedded binary — no malloc, no copies.
//

typedef struct
{
    const DictFileHeader* hdr;
    const DictWordIndex* words; // [hdr->word_count]
    const u8* entdata; // tightly-packed EntryBlob array
    const char* strpool; // null-terminated concatenated strings
} DictDB;

//
// API
//

// Open a dictionary from an in-memory blob (RC resource, mmap, etc.).
// Validates magic and version; returns a zero-filled DictDB on failure
// (caller checks db.hdr != NULL).
DictDB dict_open(const void* blob);

// Case-insensitive binary search over the alphabetically-sorted WordIndex.
// Returns word_idx (0 … hdr->word_count-1) or -1 on miss.
i32 dict_lookup(const DictDB* db, const char* word);

// Resolve a word to a headword index with variant fallback.
// 1. tries dict_lookup(word) — if found, returns the word index directly.
// 2. if not found, scans each entry's exchange (inflection) field and
//    returns the headword that lists the word as a variant.
// Returns -1 if neither the word nor any variant mapping matches.
i32 dict_resolve(const DictDB* db, const char* word);

// Convert a strpool offset to a C string pointer.
#define DICT_STR(db, off) ((const char*)((db)->strpool + (off)))

//
// Search auxiliary data — pre-built concatenated definition/example text for
// multi-threaded fuzzy search.  One entry per WordIndex slot.
//
// Separator between individual strings: 0x01 (SOH).  It is never typed by
// users and does not appear in dictionary text, so it cannot create false
// cross-segment consecutive-character bonuses.
//

typedef enum
{
    AUXSEG_DEF_EN = 0,
    AUXSEG_DEF_ZH = 1,
} AuxSegKind;

typedef struct
{
    u32 offset; // byte offset within def_search_text
    u32 len; // length in bytes (excludes the trailing separator)
    u8 kind; // AuxSegKind
    u8 def_index; // which definition/example group (0-based)
} AuxSegment;

typedef struct
{
    const char* def_search_text; // points into DictDB strpool (no copy)
    i32 def_len;
    AuxSegment* def_segs; // [def_seg_count] — arena-allocated
    i32 def_seg_count;
} DictSearchAuxEntry;

// Build the search-auxiliary array from an open DictDB.
// All memory (entry array + text buffers + segment arrays) is allocated from
// `arena`.  Returns NULL if the arena is too small.
// The returned array has db->hdr->word_count entries, indexed parallel to
// db->words[].
DictSearchAuxEntry* dict_build_search_aux(const DictDB* db, Arena* arena);

//
// EntryBlob low-level readers (used to parse entry data).
// All are static inline so the compiler can fold the tiny helpers.
//

#pragma warning(disable : 4068)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"

static inline u32 dict_rd_u32(const u8** p)
{
    u32 v;
    memcpy(&v, *p, 4);
    *p += 4;
    return v;
}

#pragma clang diagnostic pop
