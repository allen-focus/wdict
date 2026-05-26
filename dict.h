#pragma once

#include "utils.h"

//
// Constants
//

#define DICT_MAGIC   0x44494354u /* "DICT" */
#define DICT_VERSION 2u

//
// PosKind — packed as u8 in the binary blob.
//

typedef enum
{
    POS_NOUN         = 0,   // Noun
    POS_VERB         = 1,   // Verb
    POS_NOUN_VERB    = 2,   // Noun, verb
    POS_ADJ          = 3,   // Adjective
    POS_ADV          = 4,   // Adverb
    POS_ADJ_ADV      = 5,   // Adjective, adverb
    POS_CONJ         = 6,   // Conjunction
    POS_DET          = 7,   // Determiner
    POS_INDEF_ART    = 8,   // Indefinite article
    POS_INTERJ       = 9,   // Interjection
    POS_MODAL        = 10,  // Modal verb
    POS_NUM          = 11,  // Number
    POS_PREDET       = 12,  // Predeterminer
    POS_PREP         = 13,  // Preposition
    POS_ADV_PREP     = 14,  // Adverb, preposition
    POS_PRON         = 15,  // Pronoun
    POS_SUFFIX       = 16,  // Suffix
    POS_PREFIX       = 17,  // Prefix
    POS_AUX_VERB     = 18,  // Auxiliary verb
    POS_PHRASAL_VERB = 19,  // Phrasal verb
    POS_DEF_ART      = 20,  // Definite article
    POS_UNKNOWN      = 0xFF
} PosKind;

//
// On-disk binary structures (packed — no padding).
//

typedef struct
{
    u32 magic;
    u32 version;
    u32 word_count;
    u32 words_off; // → WordIndex[]
    u32 entdata_off; // → EntryData
    u32 strpool_off; // → StringPool
    u32 _reserved[2];
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

// Convert a strpool offset to a C string pointer.
#define DICT_STR(db, off) ((const char*)((db)->strpool + (off)))

//
// EntryBlob low-level readers (used by entry_view.c to parse entry data).
// All are static inline so the compiler can fold the tiny helpers.
//

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"

static inline u8 dict_rd_u8(const u8** p)
{
    return *(*p)++;
}

static inline u32 dict_rd_u32(const u8** p)
{
    u32 v;
    memcpy(&v, *p, 4);
    *p += 4;
    return v;
}

// Skip over a brief_en / brief_zh array: u8 count + count × u32 offsets.
static inline void dict_skip_brief_array(const u8** p)
{
    u8 count = dict_rd_u8(p);
    *p += (usize)count * 4;
}

#pragma clang diagnostic pop
