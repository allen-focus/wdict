#include "dict.h"

// case-insensitive ASCII compare (for binary search)
static i32 ascii_stricmp(const char* a, const char* b)
{
    while (*a && *b)
    {
        u8 ca = (u8)*a;
        u8 cb = (u8)*b;
        if (ca >= 'A' && ca <= 'Z')
            ca = (u8)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z')
            cb = (u8)(cb + ('a' - 'A'));
        if (ca != cb)
            return (i32)ca - (i32)cb;
        a++;
        b++;
    }
    return (i32)(u8)*a - (i32)(u8)*b;
}

/* Case-insensitive ASCII compare over explicit lengths (neither need be NUL-terminated). */
static i32 ascii_stricmp_n(const char* a, isize alen, const char* b, isize blen)
{
    isize n = alen < blen ? alen : blen;
    for (isize i = 0; i < n; i++)
    {
        u8 ca = (u8)a[i];
        u8 cb = (u8)b[i];
        if (ca >= 'A' && ca <= 'Z')
            ca = (u8)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z')
            cb = (u8)(cb + ('a' - 'A'));
        if (ca != cb)
            return (i32)ca - (i32)cb;
    }
    return (i32)alen - (i32)blen;
}

//
// API
//

DictDB dict_open(const void* blob)
{
    DictDB db = { 0 };
    if (!blob)
        return db;

    const u8* base = (const u8*)blob;
    const DictFileHeader* hdr = (const DictFileHeader*)base;

    if (hdr->magic != DICT_MAGIC)
        return db;
    if (hdr->version < 2 || hdr->version > DICT_VERSION)
        return db;
    if (hdr->word_count == 0)
        return db;

    db.hdr = hdr;
    db.words = (const DictWordIndex*)(base + hdr->words_off);
    db.entdata = base + hdr->entdata_off;
    db.strpool = (const char*)(base + hdr->strpool_off);

    return db;
}

i32 dict_lookup(const DictDB* db, const char* word)
{
    if (!db || !db->hdr || !word || !*word)
        return -1;

    i32 lo = 0;
    i32 hi = (i32)db->hdr->word_count - 1;

    while (lo <= hi)
    {
        i32 mid = (lo + hi) / 2;
        const char* mid_word = DICT_STR(db, db->words[mid].word_stroff);
        i32 c = ascii_stricmp(mid_word, word);
        if (c == 0)
            return mid;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }

    return -1;
}

i32 dict_resolve(const DictDB* db, const char* word)
{
    // First try direct headword lookup.
    i32 idx = dict_lookup(db, word);
    if (idx >= 0)
        return idx;

    // Not found as a headword — scan the exchange (inflection) fields of every
    // entry and resolve the word to the base form that lists it as a variant.
    // v4 stores no VariantIndex, so this is the only variant-resolution path.
    if (!db || !db->words || !word || !*word)
        return -1;

    u32 wc = db->hdr->word_count;
    for (u32 i = 0; i < wc; i++)
    {
        const DictWordIndex* w = &db->words[i];
        const u8* p = db->entdata + w->entdata_off;
        dict_rd_u32(&p); /* phonetic_off */
        dict_rd_u32(&p); /* definition_off */
        dict_rd_u32(&p); /* translation_off */
        u32 ex_off = dict_rd_u32(&p); /* exchange_off */

        if (!ex_off)
            continue;

        const char* ex = DICT_STR(db, ex_off);
        isize exlen = (isize)strlen(ex);
        if (exlen <= 0)
            continue;

        /* Exchange format: "type:variant/type:variant/...", e.g.
           "d:abandoned/p:abandoned/i:abandoning/3:abandons/s:abandons".
           The base lemma is marked "0:word". A word matches this entry as a
           variant when some non-"0" segment equals it (case-insensitive). */
        isize seg_start = 0;
        for (isize j = 0; j <= exlen; j++)
        {
            if (j == exlen || ex[j] == '/')
            {
                isize seg_len = j - seg_start;
                if (seg_len > 0)
                {
                    /* Find the first ':' separating type from variant. */
                    isize colon = seg_start;
                    while (colon < j && ex[colon] != ':')
                        colon++;
                    if (colon < j)
                    {
                        const char* variant = ex + colon + 1;
                        isize vlen = j - (colon + 1);
                        if (vlen > 0 && ascii_stricmp_n(variant, vlen, word, (isize)strlen(word)) == 0)
                            return (i32)i; /* headword i lists word as a variant */
                    }
                }
                seg_start = j + 1;
            }
        }
    }

    return -1;
}

//
// Search auxiliary builder
//

DictSearchAuxEntry* dict_build_search_aux(const DictDB* db, Arena* arena)
{
    if (!db || !db->hdr)
        return NULL;
    u32 wc = db->hdr->word_count;
    if (wc == 0)
        return NULL;

    /* Pass 1 — count segments per entry */

    MEM_TRACK("[mem] dict_build_search_aux: tmp counting arena = KB(128)\n");
    Arena tmp = arena_new(KB(128), KB(128));
    i32* def_segs_arr = arena_push(&tmp, sizeof(i32), _Alignof(i32), wc);

    i32 total_def_segs = 0;

    for (u32 i = 0; i < wc; i++)
    {
        const DictWordIndex* w = &db->words[i];
        const u8* p = db->entdata + w->entdata_off;
        i32 def_segs = 0;

        dict_rd_u32(&p); /* phonetic_off */
        u32 def_off = dict_rd_u32(&p);
        u32 tr_off = dict_rd_u32(&p);
        dict_rd_u32(&p); /* exchange_off */

        if (def_off)
            def_segs++;
        if (tr_off)
            def_segs++;

        def_segs_arr[i] = def_segs;
        total_def_segs += def_segs;
    }

    total_def_segs = total_def_segs > 0 ? total_def_segs : 1;

    /* Allocate entry array + segment pool from the caller's arena.
       def_search_text points directly into db->strpool — no string copy. */

    isize needed = (isize)(wc * sizeof(DictSearchAuxEntry) + (usize)total_def_segs * sizeof(AuxSegment));
    if (arena->pos + needed > arena->reserve_end)
    {
        arena_release(&tmp);
        return NULL;
    }

    MEM_TRACK("[mem] dict_build_search_aux: entries array = %u entries x %zu B = %zu B\n", wc,
              sizeof(DictSearchAuxEntry), (usize)wc * sizeof(DictSearchAuxEntry));
    DictSearchAuxEntry* entries = arena_push(arena, sizeof(DictSearchAuxEntry), _Alignof(DictSearchAuxEntry), wc);
    MEM_TRACK("[mem] dict_build_search_aux: def_seg_pool = %d segs x %zu B = %zu B\n", total_def_segs,
              sizeof(AuxSegment), (usize)total_def_segs * sizeof(AuxSegment));
    AuxSegment* def_seg_pool = arena_push(arena, sizeof(AuxSegment), _Alignof(AuxSegment), total_def_segs);

    /* Pass 2 — fill segment tables (offsets are strpool offsets, text lives in dict_arena) */

    i32 def_seg_off = 0;

    for (u32 i = 0; i < wc; i++)
    {
        const DictWordIndex* w = &db->words[i];
        const u8* p = db->entdata + w->entdata_off;
        i32 def_segs = def_segs_arr[i];

        entries[i].def_search_text = db->strpool;
        entries[i].def_len = 0;
        entries[i].def_segs = def_seg_pool + def_seg_off;
        entries[i].def_seg_count = def_segs;

        i32 dsi = 0;

        dict_rd_u32(&p); /* phonetic_off */
        u32 blob_def_off = dict_rd_u32(&p);
        u32 blob_tr_off = dict_rd_u32(&p);
        dict_rd_u32(&p); /* exchange_off */

        if (dsi < def_segs && blob_def_off)
        {
            const char* s = DICT_STR(db, blob_def_off);
            i32 sl = (i32)strlen(s);
            if (sl > 0)
                entries[i].def_segs[dsi++] =
                    (AuxSegment){ .offset = blob_def_off, .len = (u32)sl, .kind = AUXSEG_DEF_EN, .def_index = 0 };
        }
        if (dsi < def_segs && blob_tr_off)
        {
            const char* s = DICT_STR(db, blob_tr_off);
            i32 sl = (i32)strlen(s);
            if (sl > 0)
                entries[i].def_segs[dsi++] =
                    (AuxSegment){ .offset = blob_tr_off, .len = (u32)sl, .kind = AUXSEG_DEF_ZH, .def_index = 0 };
        }

        def_seg_off += def_segs;
    }

    arena_release(&tmp);
    return entries;
}
