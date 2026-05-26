#include "dict.h"
#include "fuzzy.h"

#include <math.h>
#include <string.h>

// ── case-insensitive ASCII compare (for binary search) ──────────────────────

static i32 ascii_stricmp(const char *a, const char *b)
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

// ── API ─────────────────────────────────────────────────────────────────────

DictDB dict_open(const void *blob)
{
    DictDB db = { 0 };
    if (!blob)
        return db;

    const u8 *base = (const u8 *)blob;
    const DictFileHeader *hdr = (const DictFileHeader *)base;

    if (hdr->magic != DICT_MAGIC)
        return db;
    if (hdr->version != DICT_VERSION)
        return db;
    if (hdr->word_count == 0)
        return db;

    db.hdr     = hdr;
    db.words   = (const DictWordIndex *)(base + hdr->words_off);
    db.entdata = base + hdr->entdata_off;
    db.strpool = (const char *)(base + hdr->strpool_off);

    return db;
}

i32 dict_lookup(const DictDB *db, const char *word)
{
    if (!db || !db->hdr || !word || !*word)
        return -1;

    i32 lo = 0;
    i32 hi = (i32)db->hdr->word_count - 1;

    while (lo <= hi)
    {
        i32 mid = (lo + hi) / 2;
        const char *mid_word = DICT_STR(db, db->words[mid].word_stroff);
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

i32 dict_fuzzy_search(const DictDB *db, const char *query, i32 *out_indices, i32 max_results, Arena *scratch)
{
    if (!db || !db->hdr || !query || !*query || max_results <= 0)
        return 0;

    String q = { (u8 *)query, (isize)strlen(query) };
    u32 word_count = db->hdr->word_count;

    /* Local top‑k heap: (index, score) pairs, insertion-sorted. */
    typedef struct
    {
        i32 idx;
        f32 score;
    } Candidate;

    Candidate *results = (Candidate *)arena_push(scratch, sizeof(Candidate), _Alignof(Candidate), (isize)max_results);
    i32 n = 0;

    for (u32 i = 0; i < word_count; i++)
    {
        const char *word = DICT_STR(db, db->words[i].word_stroff);
        String cand = { (u8 *)word, (isize)strlen(word) };

        FuzzyMatch m = fuzzy_match(q, cand, scratch);
        if (m.score >= 1e8f)
            continue;

        /* ── freq weighting ──
           Higher frequency → better (lower) score.
           log2 dampens the effect so top‑10 words aren't always the winners. */
        u32 freq = db->words[i].freq;
        if (freq != 0xFFFFFFFF && freq > 0)
            m.score *= (1.0f / log2f(2.0f + (f32)freq));

        /* Insert into sorted position (ascending score = best first). */
        i32 pos = 0;
        while (pos < n && results[pos].score < m.score)
            pos++;

        if (pos < max_results)
        {
            if (n < max_results)
                n++;
            i32 to_move = n - 1 - pos;
            if (to_move > 0)
                memmove(&results[pos + 1], &results[pos], (usize)to_move * sizeof(Candidate));
            results[pos].idx   = (i32)i;
            results[pos].score = m.score;
        }
    }

    /* Copy indices out. */
    for (i32 i = 0; i < n; i++)
        out_indices[i] = results[i].idx;

    return n;
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
static void *dict_read_file(const char *path, usize *out_size)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    DWORD hi = 0;
    DWORD lo = GetFileSize(h, &hi);
    usize size = ((usize)hi << 32) | lo;
    HANDLE map = CreateFileMappingW(h, NULL, PAGE_READONLY, 0, 0, NULL);
    void *ptr = NULL;
    if (map)
    {
        ptr = MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
        CloseHandle(map);
    }
    CloseHandle(h);
    *out_size = size;
    return ptr;
}

static void dict_close_file(void *ptr)
{
    if (ptr)
        UnmapViewOfFile(ptr);
}

int main(void)
{
    puts("=== DictDB Test ===\n");

    /* Load blob */
    usize blob_size = 0;
    void *blob = dict_read_file("data/dict.bin", &blob_size);
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
        const char *tests[] = { "aardvark", "abandon", "zzz", "AbAcUs", NULL };
        for (const char **t = tests; *t; t++)
        {
            i32 idx = dict_lookup(&db, *t);
            if (idx >= 0)
                printf("    \"%s\" -> idx=%d  word=\"%s\"\n", *t, idx, DICT_STR(&db, db.words[idx].word_stroff));
            else
                printf("    \"%s\" -> NOT FOUND\n", *t);
        }
    }

    /* Test 2: Fuzzy search */
    printf("\n[2] Fuzzy search \"aban\":\n");
    {
        Arena scratch = arena_new(MB(1));
        i32 indices[16];
        i32 n = dict_fuzzy_search(&db, "aban", indices, 16, &scratch);
        for (i32 i = 0; i < n; i++)
        {
            const DictWordIndex *wi = &db.words[indices[i]];
            const char *word = DICT_STR(&db, wi->word_stroff);
            if (wi->freq != 0xFFFFFFFF)
                printf("    [%d] %s  (freq=%u)\n", i, word, wi->freq);
            else
                printf("    [%d] %s  (freq=null)\n", i, word);
        }
        printf("    %d results\n", n);
        arena_release(&scratch);
    }

    /* Test 3: Empty query */
    printf("\n[3] Empty query:\n");
    {
        i32 indices[4];
        Arena scratch = arena_new(KB(64));
        i32 n = dict_fuzzy_search(&db, "", indices, 4, &scratch);
        printf("    %d results (expect 0)\n", n);
        arena_release(&scratch);
    }

    /* Test 4: NULL db safety */
    printf("\n[4] NULL db:\n");
    {
        i32 r = dict_lookup(NULL, "test");
        printf("    dict_lookup(NULL) = %d (expect -1)\n", r);
    }

    /* Show first few word index entries */
    printf("\n[5] First 5 WordIndex entries:\n");
    for (u32 i = 0; i < 5 && i < db.hdr->word_count; i++)
    {
        const char *word = DICT_STR(&db, db.words[i].word_stroff);
        u32 freq = db.words[i].freq;
        printf("    [%u] \"%s\"  entdata_off=%u  freq=%s\n",
               i, word, db.words[i].entdata_off,
               (freq == 0xFFFFFFFF) ? "null" : "");
        if (freq != 0xFFFFFFFF)
            printf("           freq=%u\n", freq);
    }

    /* Test 6: Verify all strpool strings are valid */
    printf("\n[6] Verifying all word strings accessible:\n");
    {
        i32 ok = 0;
        for (u32 i = 0; i < db.hdr->word_count; i++)
        {
            const char *word = DICT_STR(&db, db.words[i].word_stroff);
            if (word && strlen(word) > 0)
                ok++;
        }
        printf("    %d/%u words have valid strpool references\n", ok, db.hdr->word_count);
    }

    dict_close_file(blob);

    puts("\n=== All tests complete ===");
    return 0;
}

#endif /* DICT_STANDALONE_TEST */
