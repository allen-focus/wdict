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
    db.variants = (hdr->version >= 3 && hdr->variant_off > 0 && hdr->variant_count > 0)
                      ? (const DictVariantEntry*)(base + hdr->variant_off)
                      : NULL;

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

    // Not found as a headword — try variant resolution.
    if (!db || !db->variants || !word || !*word)
        return -1;

    i32 lo = 0;
    i32 hi = (i32)db->hdr->variant_count - 1;

    while (lo <= hi)
    {
        i32 mid = (lo + hi) / 2;
        const char* vw = DICT_STR(db, db->variants[mid].variant_stroff);
        i32 c = ascii_stricmp(vw, word);
        if (c == 0)
            return (i32)db->variants[mid].base_word_idx;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
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

    /* ------------------------------------------------------------------ */
    /* Pass 1 — count total bytes and segments per entry                   */
    /* ------------------------------------------------------------------ */

    Arena tmp = arena_new(MB(8), MB(8));
    i32* def_bytes_arr = arena_push(&tmp, sizeof(i32), _Alignof(i32), wc);
    i32* ex_bytes_arr = arena_push(&tmp, sizeof(i32), _Alignof(i32), wc);
    i32* def_segs_arr = arena_push(&tmp, sizeof(i32), _Alignof(i32), wc);
    i32* ex_segs_arr = arena_push(&tmp, sizeof(i32), _Alignof(i32), wc);

    usize total_def_bytes = 0;
    usize total_ex_bytes = 0;
    i32 total_def_segs = 0;
    i32 total_ex_segs = 0;

    for (u32 i = 0; i < wc; i++)
    {
        const DictWordIndex* w = &db->words[i];
        const u8* p = db->entdata + w->entdata_off;
        i32 def_bytes = 0, ex_bytes = 0;
        i32 def_segs = 0, ex_segs = 0;

        p += 4; /* skip freq */

        /* brief_en — skip (fast gloss, not part of definition search) */
        {
            u8 cnt = *p++;
            for (u8 j = 0; j < cnt; j++)
                dict_rd_u32(&p);
        }

        /* brief_zh — skip */
        {
            u8 cnt = *p++;
            for (u8 j = 0; j < cnt; j++)
                dict_rd_u32(&p);
        }

        /* POS sections */
        u8 pos_count = *p++;
        for (u8 pi = 0; pi < pos_count; pi++)
        {
            p++; /* skip pos_kind */
            p += 4; /* skip pron_off */
            u8 def_count = *p++;
            for (u8 di = 0; di < def_count; di++)
            {
                u32 en_off = dict_rd_u32(&p);
                u32 zh_off = dict_rd_u32(&p);

                def_bytes += (i32)strlen(DICT_STR(db, en_off)) + 1;
                def_segs++;
                def_bytes += (i32)strlen(DICT_STR(db, zh_off)) + 1;
                def_segs++;

                u8 ex_count = *p++;
                for (u8 ei = 0; ei < ex_count; ei++)
                {
                    u32 ex_en_off = dict_rd_u32(&p);
                    u32 ex_zh_off = dict_rd_u32(&p);

                    ex_bytes += (i32)strlen(DICT_STR(db, ex_en_off)) + 1;
                    ex_segs++;
                    ex_bytes += (i32)strlen(DICT_STR(db, ex_zh_off)) + 1;
                    ex_segs++;
                }
            }
        }

        def_bytes_arr[i] = def_bytes;
        ex_bytes_arr[i] = ex_bytes;
        def_segs_arr[i] = def_segs;
        ex_segs_arr[i] = ex_segs;
        total_def_bytes += (usize)def_bytes;
        total_ex_bytes += (usize)ex_bytes;
        total_def_segs += def_segs;
        total_ex_segs += ex_segs;
    }

    /* Ensure at least 1 byte/slot so pointers are always valid */
    total_def_bytes = total_def_bytes > 0 ? total_def_bytes : 1;
    total_ex_bytes = total_ex_bytes > 0 ? total_ex_bytes : 1;
    total_def_segs = total_def_segs > 0 ? total_def_segs : 1;
    total_ex_segs = total_ex_segs > 0 ? total_ex_segs : 1;

    /* ------------------------------------------------------------------ */
    /* Allocate all data from the caller's arena                           */
    /* ------------------------------------------------------------------ */

    isize needed = (isize)(wc * sizeof(DictSearchAuxEntry) + total_def_bytes + total_ex_bytes +
                           (usize)total_def_segs * sizeof(AuxSegment) +
                           (usize)total_ex_segs * sizeof(AuxSegment));
    if (arena->pos + needed > arena->reserve_end)
    {
        arena_release(&tmp);
        return NULL;
    }

    DictSearchAuxEntry* entries =
        arena_push(arena, sizeof(DictSearchAuxEntry), _Alignof(DictSearchAuxEntry), wc);
    char* def_buf = arena_push(arena, 1, 1, total_def_bytes);
    char* ex_buf = arena_push(arena, 1, 1, total_ex_bytes);
    AuxSegment* def_seg_pool =
        arena_push(arena, sizeof(AuxSegment), _Alignof(AuxSegment), total_def_segs);
    AuxSegment* ex_seg_pool =
        arena_push(arena, sizeof(AuxSegment), _Alignof(AuxSegment), total_ex_segs);

    /* ------------------------------------------------------------------ */
    /* Pass 2 — fill strings and segment tables                             */
    /* ------------------------------------------------------------------ */

    usize def_off = 0, ex_off = 0;
    i32 def_seg_off = 0, ex_seg_off = 0;

    for (u32 i = 0; i < wc; i++)
    {
        const DictWordIndex* w = &db->words[i];
        const u8* p = db->entdata + w->entdata_off;
        i32 def_bytes = def_bytes_arr[i];
        i32 ex_bytes = ex_bytes_arr[i];
        i32 def_segs = def_segs_arr[i];
        i32 ex_segs = ex_segs_arr[i];

        entries[i].def_search_text = def_buf + def_off;
        entries[i].ex_search_text = ex_buf + ex_off;
        entries[i].def_len = def_bytes;
        entries[i].ex_len = ex_bytes;
        entries[i].def_segs = def_seg_pool + def_seg_off;
        entries[i].ex_segs = ex_seg_pool + ex_seg_off;
        entries[i].def_seg_count = def_segs;
        entries[i].ex_seg_count = ex_segs;

        char* dptr = entries[i].def_search_text;
        char* xptr = entries[i].ex_search_text;
        u32 def_cur = 0, ex_cur = 0;
        i32 dsi = 0, xsi = 0;

        p += 4; /* skip freq */

        /* brief_en — skip (fast gloss, not part of definition search) */
        {
            u8 cnt = *p++;
            for (u8 j = 0; j < cnt; j++)
                dict_rd_u32(&p);
        }

        /* brief_zh — skip */
        {
            u8 cnt = *p++;
            for (u8 j = 0; j < cnt; j++)
                dict_rd_u32(&p);
        }

        /* POS sections */
        u8 pos_count = *p++;
        for (u8 pi = 0; pi < pos_count; pi++)
        {
            p++; /* skip pos_kind */
            p += 4; /* skip pron_off */
            u8 def_count = *p++;
            for (u8 di = 0; di < def_count; di++)
            {
                u32 en_off = dict_rd_u32(&p);
                u32 zh_off = dict_rd_u32(&p);

                {
                    const char* s = DICT_STR(db, en_off);
                    i32 sl = (i32)strlen(s);
                    if (dsi < def_segs)
                    {
                        memcpy(dptr, s, sl);
                        dptr[sl] = DICT_SEARCH_SEP_CHAR;
                        entries[i].def_segs[dsi++] = (AuxSegment){
                            .offset = def_cur, .len = (u32)sl, .kind = AUXSEG_DEF_EN, .def_index = di };
                        dptr += sl + 1;
                        def_cur += (u32)sl + 1;
                    }
                }

                {
                    const char* s = DICT_STR(db, zh_off);
                    i32 sl = (i32)strlen(s);
                    if (dsi < def_segs)
                    {
                        memcpy(dptr, s, sl);
                        dptr[sl] = DICT_SEARCH_SEP_CHAR;
                        entries[i].def_segs[dsi++] = (AuxSegment){
                            .offset = def_cur, .len = (u32)sl, .kind = AUXSEG_DEF_ZH, .def_index = di };
                        dptr += sl + 1;
                        def_cur += (u32)sl + 1;
                    }
                }

                u8 ex_count = *p++;
                for (u8 ei = 0; ei < ex_count; ei++)
                {
                    u32 ex_en_off = dict_rd_u32(&p);
                    u32 ex_zh_off = dict_rd_u32(&p);

                    {
                        const char* s = DICT_STR(db, ex_en_off);
                        i32 sl = (i32)strlen(s);
                        if (xsi < ex_segs)
                        {
                            memcpy(xptr, s, sl);
                            xptr[sl] = DICT_SEARCH_SEP_CHAR;
                            entries[i].ex_segs[xsi++] = (AuxSegment){
                                .offset = ex_cur, .len = (u32)sl, .kind = AUXSEG_EX_EN, .def_index = di };
                            xptr += sl + 1;
                            ex_cur += (u32)sl + 1;
                        }
                    }

                    {
                        const char* s = DICT_STR(db, ex_zh_off);
                        i32 sl = (i32)strlen(s);
                        if (xsi < ex_segs)
                        {
                            memcpy(xptr, s, sl);
                            xptr[sl] = DICT_SEARCH_SEP_CHAR;
                            entries[i].ex_segs[xsi++] = (AuxSegment){
                                .offset = ex_cur, .len = (u32)sl, .kind = AUXSEG_EX_ZH, .def_index = di };
                            xptr += sl + 1;
                            ex_cur += (u32)sl + 1;
                        }
                    }
                }
            }
        }

        def_off += (usize)def_bytes;
        ex_off += (usize)ex_bytes;
        def_seg_off += def_segs;
        ex_seg_off += ex_segs;
    }

    arena_release(&tmp);
    return entries;
}

//
// Standalone test
//

#ifdef DICT_STANDALONE_TEST

#    define _CRT_SECURE_NO_WARNINGS
#    include <stdio.h>
#    include <windows.h>

/* Load a file via memory-mapping for standalone testing.
   In the real build the blob is embedded via resource.rc. */
static void* dict_read_file(const char* path, usize* out_size)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    DWORD hi = 0;
    DWORD lo = GetFileSize(h, &hi);
    usize size = ((usize)hi << 32) | lo;
    HANDLE map = CreateFileMappingW(h, NULL, PAGE_READONLY, 0, 0, NULL);
    void* ptr = NULL;
    if (map)
    {
        ptr = MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
        CloseHandle(map);
    }
    CloseHandle(h);
    *out_size = size;
    return ptr;
}

static void dict_close_file(void* ptr)
{
    if (ptr)
        UnmapViewOfFile(ptr);
}

int main(void)
{
    puts("=== DictDB Test ===\n");

    /* Load blob */
    usize blob_size = 0;
    void* blob = dict_read_file("data/dict.bin", &blob_size);
    if (!blob)
    {
        puts("FATAL: cannot open data/dict.bin");
        return 1;
    }
    printf("Loaded %zu bytes\n", blob_size);

    /* Open database */
    DictDB db = dict_open(blob);
    if (!db.hdr)
    {
        puts("FATAL: dict_open failed (bad magic/version?)");
        dict_close_file(blob);
        return 1;
    }
    printf("Dictionary: %u words\n\n", db.hdr->word_count);

    /* Test 1: Exact lookup */
    printf("[1] Exact lookups:\n");
    {
        const char* tests[] = { "aardvark", "abandon", "zzz", "AbAcUs", NULL };
        for (const char** t = tests; *t; t++)
        {
            i32 idx = dict_lookup(&db, *t);
            if (idx >= 0)
                printf("    \"%s\" -> idx=%d  word=\"%s\"\n", *t, idx, DICT_STR(&db, db.words[idx].word_stroff));
            else
                printf("    \"%s\" -> NOT FOUND\n", *t);
        }
    }

    /* Test 2: NULL db safety */
    printf("\n[2] NULL db:\n");
    {
        i32 r = dict_lookup(NULL, "test");
        printf("    dict_lookup(NULL) = %d (expect -1)\n", r);
    }

    /* Test 3: First 5 WordIndex entries */
    printf("\n[3] First 5 WordIndex entries:\n");
    for (u32 i = 0; i < 5 && i < db.hdr->word_count; i++)
    {
        const char* word = DICT_STR(&db, db.words[i].word_stroff);
        u32 freq = db.words[i].freq;
        printf("    [%u] \"%s\"  entdata_off=%u  freq=%s\n", i, word, db.words[i].entdata_off,
               (freq == 0xFFFFFFFF) ? "null" : "");
        if (freq != 0xFFFFFFFF)
            printf("           freq=%u\n", freq);
    }

    /* Test 4: Verify all strpool strings are valid */
    printf("\n[4] Verifying all word strings accessible:\n");
    {
        i32 ok = 0;
        for (u32 i = 0; i < db.hdr->word_count; i++)
        {
            const char* word = DICT_STR(&db, db.words[i].word_stroff);
            if (word && strlen(word) > 0)
                ok++;
        }
        printf("    %d/%u words have valid strpool references\n", ok, db.hdr->word_count);
    }

    /* ================================================================== */
    /* Test 5-9: Search auxiliary (dict_build_search_aux)                  */
    /* ================================================================== */

    Arena aux_arena = arena_new(MB(128), MB(8));
    DictSearchAuxEntry* aux = dict_build_search_aux(&db, &aux_arena);
    if (!aux)
    {
        puts("FATAL: dict_build_search_aux returned NULL (arena too small?)");
        arena_release(&aux_arena);
        dict_close_file(blob);
        return 1;
    }

    /* Test 5: Basic statistics */
    printf("\n[5] Search auxiliary statistics:\n");
    {
        usize total_def_bytes = 0, total_ex_bytes = 0;
        i32 total_def_segs = 0, total_ex_segs = 0;
        i32 max_def_segs = 0, max_ex_segs = 0;
        i32 empty_def = 0, empty_ex = 0;

        for (u32 i = 0; i < db.hdr->word_count; i++)
        {
            DictSearchAuxEntry* e = &aux[i];
            total_def_bytes += (usize)e->def_len;
            total_ex_bytes += (usize)e->ex_len;
            total_def_segs += e->def_seg_count;
            total_ex_segs += e->ex_seg_count;
            if (e->def_seg_count > max_def_segs)
                max_def_segs = e->def_seg_count;
            if (e->ex_seg_count > max_ex_segs)
                max_ex_segs = e->ex_seg_count;
            if (e->def_len == 0)
                empty_def++;
            if (e->ex_len == 0)
                empty_ex++;
        }

        printf("    def_search_text total bytes:     %zu (%.1f MB)\n", total_def_bytes,
               (f64)total_def_bytes / MB(1));
        printf("    ex_search_text  total bytes:     %zu (%.1f MB)\n", total_ex_bytes,
               (f64)total_ex_bytes / MB(1));
        printf("    def_segs total count:            %d\n", total_def_segs);
        printf("    ex_segs total count:             %d\n", total_ex_segs);
        printf("    max def_segs per entry:          %d\n", max_def_segs);
        printf("    max ex_segs per entry:           %d\n", max_ex_segs);
        printf("    entries with empty def_text:     %d\n", empty_def);
        printf("    entries with empty ex_text:      %d\n", empty_ex);
        printf("    arena memory used:               ~%.1f MB\n",
               (f64)(total_def_bytes + total_ex_bytes + (usize)total_def_segs * sizeof(AuxSegment) +
                     (usize)total_ex_segs * sizeof(AuxSegment) + (usize)db.hdr->word_count * sizeof(DictSearchAuxEntry)) /
                   MB(1));
    }

    /* Test 6: Look up "abandon" and dump its aux data */
    printf("\n[6] Aux data for \"abandon\":\n");
    {
        i32 idx = dict_lookup(&db, "abandon");
        if (idx < 0)
        {
            puts("    \"abandon\" not found in dictionary!");
        }
        else
        {
            DictSearchAuxEntry* e = &aux[idx];

            printf("    idx=%d  def_len=%d  ex_len=%d  def_segs=%d  ex_segs=%d\n", idx, e->def_len, e->ex_len,
                   e->def_seg_count, e->ex_seg_count);

            printf("\n    def_search_text (first 200 chars):\n    ");
            for (i32 j = 0; j < e->def_len && j < 200; j++)
            {
                char c = e->def_search_text[j];
                if (c == DICT_SEARCH_SEP_CHAR)
                    printf("|");
                else
                    putchar(c);
            }
            printf("\n");

            printf("\n    ex_search_text (first 200 chars):\n    ");
            for (i32 j = 0; j < e->ex_len && j < 200; j++)
            {
                char c = e->ex_search_text[j];
                if (c == DICT_SEARCH_SEP_CHAR)
                    printf("|");
                else
                    putchar(c);
            }
            printf("\n");

            printf("\n    def_segs (%d total):\n", e->def_seg_count);
            for (i32 j = 0; j < e->def_seg_count && j < 8; j++)
            {
                AuxSegment* seg = &e->def_segs[j];
                printf("      [off=%u len=%u kind=%u def_idx=%u]\n", seg->offset, seg->len, seg->kind,
                       seg->def_index);
            }
            if (e->def_seg_count > 8)
                printf("      ... (%d more)\n", e->def_seg_count - 8);

            printf("\n    ex_segs (%d total):\n", e->ex_seg_count);
            for (i32 j = 0; j < e->ex_seg_count && j < 4; j++)
            {
                AuxSegment* seg = &e->ex_segs[j];
                printf("      [off=%u len=%u kind=%u def_idx=%u]\n", seg->offset, seg->len, seg->kind,
                       seg->def_index);
            }
            if (e->ex_seg_count > 4)
                printf("      ... (%d more)\n", e->ex_seg_count - 4);
        }
    }

    /* Test 7: Check "disregard" contains expected text in def */
    printf("\n[7] Fuzzy-def-search sanity check \"disregard\":\n");
    {
        i32 idx = dict_lookup(&db, "disregard");
        if (idx >= 0)
        {
            DictSearchAuxEntry* e = &aux[idx];
            /* Build a lowercase copy for strstr */
            char* lower = (char*)malloc((usize)e->def_len + 1);
            for (i32 j = 0; j < e->def_len; j++)
            {
                char c = e->def_search_text[j];
                lower[j] = (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
            }
            lower[e->def_len] = '\0';

            const char* checks[] = {"neglect", "ignore", "disregard", NULL};
            for (const char** ck = checks; *ck; ck++)
            {
                b32 found = strstr(lower, *ck) != NULL;
                printf("    def_text contains \"%s\": %s\n", *ck, found ? "yes" : "NO");
            }
            free(lower);
        }
    }

    /* Test 8: Verify seg offsets are monotonic and within bounds */
    printf("\n[8] Segment offset consistency (sampling 1000 entries):\n");
    {
        i32 errors = 0;
        i32 sampled = 0;
        u32 step = db.hdr->word_count / 1000;
        if (step < 1)
            step = 1;
        for (u32 i = 0; i < db.hdr->word_count; i += step)
        {
            sampled++;
            DictSearchAuxEntry* e = &aux[i];
            /* def_segs */
            for (i32 j = 0; j < e->def_seg_count; j++)
            {
                AuxSegment* seg = &e->def_segs[j];
                if (seg->offset + seg->len > (u32)e->def_len)
                    errors++;
            }
            /* ex_segs */
            for (i32 j = 0; j < e->ex_seg_count; j++)
            {
                AuxSegment* seg = &e->ex_segs[j];
                if (seg->offset + seg->len > (u32)e->ex_len)
                    errors++;
            }
        }
        printf("    %d segment range errors in %d entries (expect 0)\n", errors, sampled);
    }

    /* Test 9: First 10 entries — basic sanity */
    printf("\n[9] First 10 entries aux summary:\n");
    for (u32 i = 0; i < 10 && i < db.hdr->word_count; i++)
    {
        DictSearchAuxEntry* e = &aux[i];
        const char* word = DICT_STR(&db, db.words[i].word_stroff);
        printf("    [%u] \"%s\"  def=%d bytes/%d segs  ex=%d bytes/%d segs\n", i, word, e->def_len,
               e->def_seg_count, e->ex_len, e->ex_seg_count);
    }

    arena_release(&aux_arena);
    dict_close_file(blob);

    puts("\n=== All tests complete ===");
    return 0;
}

#endif /* DICT_STANDALONE_TEST */
