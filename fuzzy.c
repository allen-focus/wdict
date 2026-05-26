#include "fuzzy.h"
#include <math.h>

//
// Scoring tunables
//

#define FUZZY_GAP_PENALTY         2.0f
#define FUZZY_CONSECUTIVE_BONUS   3.0f
#define FUZZY_WORD_BOUNDARY_BONUS 2.0f
#define FUZZY_CAMEL_BONUS         1.0f
#define FUZZY_FIRST_CHAR_BONUS    2.0f

//
// Character classification (no utils equivalents)
//

static u32 to_lower_cp(u32 cp)
{
    if (cp >= 'A' && cp <= 'Z')
        return cp + ('a' - 'A');
    return cp;
}

static b32 is_upper_cp(u32 cp)
{
    return cp >= 'A' && cp <= 'Z';
}

static b32 is_cjk_cp(u32 cp)
{
    return (cp >= 0x4E00 && cp <= 0x9FFF) || // CJK Unified Ideographs
           (cp >= 0x3400 && cp <= 0x4DBF) || // CJK Ext-A
           (cp >= 0x20000 && cp <= 0x2A6DF) || // CJK Ext-B
           (cp >= 0xF900 && cp <= 0xFAFF) || // CJK Compat
           (cp >= 0x2F800 && cp <= 0x2FA1F); // CJK Compat Suppl
}

static b32 is_alnum_cp(u32 cp)
{
    return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9');
}

// Walk backwards from byte_offset to find the start of the containing
// UTF-8 character.  Continuation bytes have pattern 10xxxxxx (0x80–0xBF).
// (No utils equivalent for reverse UTF-8 scanning.)
static i32 prev_char_start(const u8* text, isize byte_offset)
{
    i32 off = (i32)byte_offset - 1;
    while (off > 0 && (text[off] & 0xC0) == 0x80)
        off--;
    return off;
}

// Detects whether position `byte_offset` is a "word start" in `text`.
// Word starts are: start-of-string, after whitespace/separators, after CJK,
// camelCase uppercase, transition non-alnum→alnum.
static b32 is_word_start(const u8* text, isize len, isize byte_offset)
{
    if (byte_offset == 0)
        return True;
    if (byte_offset >= len)
        return False;

    i32 prev_start = prev_char_start(text, byte_offset);
    u32 prev_cp = utf8_decode(&text[prev_start]).codepoint;
    u32 cur_cp = utf8_decode(&text[byte_offset]).codepoint;

    // CJK characters always form their own "word" boundary
    if (is_cjk_cp(cur_cp))
        return True;
    if (is_cjk_cp(prev_cp))
        return True;

    // Separators before current char
    if (prev_cp == ' ' || prev_cp == '-' || prev_cp == '_' || prev_cp == '/' || prev_cp == '\\' || prev_cp == '.' ||
        prev_cp == ',' || prev_cp == '(' || prev_cp == '[' || prev_cp == '{' || prev_cp == ':')
        return True;

    // camelCase: lowercase→uppercase transition
    if (is_upper_cp(cur_cp) && !is_upper_cp(to_lower_cp(prev_cp)) && is_alnum_cp(prev_cp))
        return True;

    // Non-alnum → alnum transition
    if (is_alnum_cp(cur_cp) && !is_alnum_cp(prev_cp))
        return True;

    return False;
}

//
// Internal: per-character info built during matching
//

typedef struct
{
    i32 byte_offset;
    i32 byte_len;
    u32 cp; // lowercased codepoint (for case-insensitive comparison)
    b32 is_word_start;
} CharInfo;

// Decode the full string into a CharInfo array.
static i32 decode_string(String s, CharInfo* out, i32 cap, b32 want_word_start)
{
    i32 count = 0;
    isize i = 0;
    while (i < s.len && count < cap)
    {
        UnicodeDecode ud = utf8_decode(&s.data[i]);
        i32 blen = (i32)(ud.next_p - &s.data[i]);
        out[count++] = (CharInfo){
            .byte_offset = (i32)i,
            .byte_len = blen,
            .cp = to_lower_cp(ud.codepoint),
            .is_word_start = want_word_start ? is_word_start(s.data, s.len, i) : False,
        };
        i += blen;
    }
    return count;
}

//
// Public API
//

#define FUZZY_DP_INF 1e9f // DP sentinel — any score below this is a valid match

// Helpers for clean indexing
#define DP(q, c)  dp[(usize)(q) * (usize)c_count + (usize)(c)]
#define PAR(q, c) parent[(usize)(q) * (usize)c_count + (usize)(c)]

FuzzyMatch fuzzy_match(String query, String candidate, Arena* scratch)
{
    FuzzyMatch result = { .score = FUZZY_DP_INF };
    isize scratch_pos = scratch->pos;

    if (query.len == 0)
    {
        result.score = 0.0f;
        arena_pop_to(scratch, scratch_pos);
        return result;
    }
    if (candidate.len == 0)
    {
        arena_pop_to(scratch, scratch_pos);
        return result;
    }

    /* decode query & candidate into codepoint arrays (stack-allocated, small) */
    CharInfo q_chars[FUZZY_MAX_QUERY_CHARS];
    CharInfo c_chars[FUZZY_MAX_CANDIDATE_CHARS];
    i32 q_count = decode_string(query, q_chars, FUZZY_MAX_QUERY_CHARS, False);
    i32 c_count = decode_string(candidate, c_chars, FUZZY_MAX_CANDIDATE_CHARS, True);

    if (q_count == 0)
    {
        result.score = 0.0f;
        arena_pop_to(scratch, scratch_pos);
        return result;
    }
    if (c_count < q_count)
    {
        arena_pop_to(scratch, scratch_pos);
        return result;
    }

    /* Allocate DP tables from arena (1-D with manual 2-D indexing).
       parent is i32 — wider than i16 — to eliminate any overflow risk. */
    usize dp_cells = (usize)q_count * (usize)c_count;
    f32* dp = arena_push(scratch, sizeof(f32), 4, dp_cells);
    i32* parent = arena_push(scratch, sizeof(i32), 4, dp_cells);

    f32 best_score = FUZZY_DP_INF;
    i32 best_positions[FUZZY_MAX_QUERY_CHARS];
    i32 best_count = 0;

    u32 first_q_cp = q_chars[0].cp;

    /* ---- per-start DP: find optimal character alignment ------------------ */
    for (i32 start = 0; start <= c_count - q_count; start++)
    {
        if (c_chars[start].cp != first_q_cp)
            continue;

        /* Clear the rows this start iteration will touch */
        for (i32 q = 0; q < q_count; q++)
            for (i32 c = start; c < c_count; c++)
                DP(q, c) = FUZZY_DP_INF;

        /* Base case: match query[0] at candidate[start] */
        {
            f32 base = (f32)start; // position penalty
            if (start == 0)
                base -= FUZZY_FIRST_CHAR_BONUS;
            if (c_chars[start].is_word_start)
                base -= FUZZY_WORD_BOUNDARY_BONUS;
            {
                UnicodeDecode ud = utf8_decode(&candidate.data[c_chars[start].byte_offset]);
                if (is_upper_cp(ud.codepoint))
                    base -= FUZZY_CAMEL_BONUS;
            }
            DP(0, start) = base;
            PAR(0, start) = -1;
        }

        /* ---- fill DP row-by-row ------------------------------------------ */
        for (i32 q = 1; q < q_count; q++)
        {
            u32 target_cp = q_chars[q].cp;

            // Running minimum of  DP(q-1,prev) - (prev+1) * GAP_PENALTY
            // for the non‑adjacent (gap ≥ 1) transition.
            // The list accumulates via the pool‑update at the bottom of
            // the c‑loop; it is pre‑seeded with `start` (the base‑case
            // position) so the first few c values already have a valid prev.
            f32 best_adj = FUZZY_DP_INF;
            i32 best_adj_prev = -1;

            /* Pre‑seed the non‑adjacent pool with the base‑case position */
            if (DP(q - 1, start) < FUZZY_DP_INF)
            {
                f32 adj = DP(q - 1, start) - (f32)(start + 1) * FUZZY_GAP_PENALTY;
                best_adj = adj;
                best_adj_prev = start;
            }

            /* c‑loop starts at start+1 so the pool naturally accumulates
               every valid prev position through its bottom‑of‑loop update */
            for (i32 c = start + 1; c < c_count; c++)
            {
                /* Only attempt to match when we have enough room for the
                   needed number of characters (c ≥ q). */
                if (c >= q && c_chars[c].cp == target_cp)
                {
                    /* ---- adjacent transition (prev = c-1, gap = 0) ---- */
                    {
                        i32 prev = c - 1;
                        if (DP(q - 1, prev) < FUZZY_DP_INF)
                        {
                            f32 s = DP(q - 1, prev) - FUZZY_CONSECUTIVE_BONUS;
                            if (c_chars[c].is_word_start)
                                s -= FUZZY_WORD_BOUNDARY_BONUS;
                            {
                                UnicodeDecode ud = utf8_decode(&candidate.data[c_chars[c].byte_offset]);
                                if (is_upper_cp(ud.codepoint))
                                    s -= FUZZY_CAMEL_BONUS;
                            }
                            if (s < DP(q, c))
                            {
                                DP(q, c) = s;
                                PAR(q, c) = prev;
                            }
                        }
                    }

                    /* ---- non-adjacent transition (prev ≤ c-2, gap ≥ 1) ---- */
                    if (best_adj < FUZZY_DP_INF)
                    {
                        f32 s = best_adj + (f32)c * FUZZY_GAP_PENALTY;
                        if (c_chars[c].is_word_start)
                            s -= FUZZY_WORD_BOUNDARY_BONUS;
                        {
                            UnicodeDecode ud = utf8_decode(&candidate.data[c_chars[c].byte_offset]);
                            if (is_upper_cp(ud.codepoint))
                                s -= FUZZY_CAMEL_BONUS;
                        }
                        if (s < DP(q, c))
                        {
                            DP(q, c) = s;
                            PAR(q, c) = best_adj_prev;
                        }
                    }
                }

                /* Add candidate position c to the non‑adjacent pool for
                   future c values (it will be a valid prev for c ≥ c+2). */
                if (DP(q - 1, c) < FUZZY_DP_INF)
                {
                    f32 adj = DP(q - 1, c) - (f32)(c + 1) * FUZZY_GAP_PENALTY;
                    if (adj < best_adj)
                    {
                        best_adj = adj;
                        best_adj_prev = c;
                    }
                }
            }
        }

        /* Pick the best end position for this start */
        for (i32 c = start + q_count - 1; c < c_count; c++)
        {
            if (DP(q_count - 1, c) < best_score)
            {
                best_score = DP(q_count - 1, c);
                best_count = q_count;
                /* Backtrack to recover the full position array */
                {
                    i32 idx = c;
                    for (i32 qi = q_count - 1; qi >= 0; qi--)
                    {
                        best_positions[qi] = idx;
                        idx = PAR(qi, idx);
                    }
                }
            }
        }
    }

    if (best_score >= 1e8f)
    {
        arena_pop_to(scratch, scratch_pos);
        return result; // no match found
    }

    /* build match ranges (merge adjacent byte ranges) */
    result.score = best_score;
    result.range_count = 0;

    i32 rstart = c_chars[best_positions[0]].byte_offset;
    i32 rend = rstart + c_chars[best_positions[0]].byte_len;

    for (i32 i = 1; i < best_count; i++)
    {
        CharInfo* ci = &c_chars[best_positions[i]];
        if (ci->byte_offset == rend)
        {
            rend = ci->byte_offset + ci->byte_len;
        }
        else
        {
            if (result.range_count < FUZZY_MAX_RANGES)
                result.ranges[result.range_count++] = (FuzzyRange){ rstart, rend };
            rstart = ci->byte_offset;
            rend = ci->byte_offset + ci->byte_len;
        }
    }
    if (result.range_count < FUZZY_MAX_RANGES)
        result.ranges[result.range_count++] = (FuzzyRange){ rstart, rend };

    arena_pop_to(scratch, scratch_pos);
    return result;
}

//
// Standalone test
//

#ifdef FUZZY_STANDALONE_TEST

#    include <stdio.h>

static void print_highlighted(String cand, FuzzyRange* ranges, i32 count)
{
    i32 ri = 0;
    isize i = 0;
    while (i < cand.len)
    {
        if (ri < count && (isize)ranges[ri].start == i)
        {
            fputs("\x1b[35;1m", stdout);
            for (; i < cand.len && i < (isize)ranges[ri].end; i++)
                putc(cand.data[i], stdout);
            fputs("\x1b[0m", stdout);
            ri++;
        }
        else
        {
            putc(cand.data[i], stdout);
            i++;
        }
    }
}

static void run_test(const char* name, const char* q, const char* c, Arena* scratch)
{
    String query = { (u8*)(q), (isize)strlen(q) };
    String candidate = { (u8*)(c), (isize)strlen(c) };

    FuzzyMatch m = fuzzy_match(query, candidate, scratch);

    printf("%-30s  query=\"%s\"  cand=\"%s\"\n", name, q, c);
    if (m.score >= 1e8f)
    {
        printf("  -> NO MATCH\n\n");
        return;
    }

    printf("  -> score=%.1f  ranges=%d  | ", m.score, m.range_count);
    print_highlighted(candidate, m.ranges, m.range_count);

    printf("\n  ranges:");
    for (i32 i = 0; i < m.range_count; i++)
        printf(" [%d,%d)", m.ranges[i].start, m.ranges[i].end);
    printf("\n\n");
}

int main(void)
{
    Arena scratch = arena_new(MB(1));

    printf("=== Fuzzy Match Tests ===\n\n");

    run_test("exact", "linear", "linear", &scratch);
    run_test("substring", "ear", "linear", &scratch);
    run_test("fuzzy", "lnr", "linear", &scratch);
    run_test("case insensitive", "LIN", "linear", &scratch);
    run_test("scattered", "lia", "linear", &scratch);
    run_test("camelCase", "GSB", "getScrollBarWidth", &scratch);
    run_test("camelCase partial", "SBW", "getScrollBarWidth", &scratch);
    run_test("path-style", "fzm", "fuzzy_match", &scratch);
    run_test("no match", "xyz", "linear", &scratch);
    run_test("empty query", "", "linear", &scratch);
    run_test("empty candidate", "hello", "", &scratch);
    run_test("query longer", "longword", "short", &scratch);

    run_test("Chinese exact", "中国", "中国", &scratch);
    run_test("Chinese fuzzy", "中国", "中国字典", &scratch);
    run_test("Chinese mixed", "中文混合", "中英文混合翻译", &scratch);
    run_test("Chinese start", "字典", "字典", &scratch);
    run_test("Chinese single", "字", "中国字典", &scratch);
    run_test("Pinyin-like chars", "zd", "字典", &scratch);
    run_test("mixed en-cn", "中en", "中英文en测试", &scratch);
    run_test("partial en-cn", "文te", "中文test", &scratch);

    run_test("word boundary exact", "app", "getAppConfig", &scratch);
    run_test("word boundary fuzzy", "gAC", "getAppConfig", &scratch);
    run_test("underscore sep", "gsb", "get_scroll_bar", &scratch);
    run_test("hyphen sep", "gsb", "get-scroll-bar", &scratch);

    // DP-specific: greedy would pick b at position 2 (close to a), forcing
    // a large gap to c.  DP sees that skipping to b at 4 reduces total cost.
    run_test("dp skip closer match", "abc", "axbxbc", &scratch);

    // DP-specific: all repeated char, verify optimal alignment in presence
    // of many identical candidates.
    run_test("dp repeated chars", "aaa", "aXaYaZa", &scratch);

    arena_release(&scratch);

    printf("=== All tests complete ===\n");
    return 0;
}

#endif // FUZZY_STANDALONE_TEST

//
// Benchmark
//

#ifdef FUZZY_BENCH

#    include <stdio.h>
#    include <windows.h>
#    include <string.h>
#    include "dict.h"

static void* bench_read_file(const char* path, usize* out_size)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
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

static void bench_close_file(void* ptr)
{
    if (ptr)
        UnmapViewOfFile(ptr);
}

int main(void)
{
    usize blob_size = 0;
    void* blob = bench_read_file("data/dict.bin", &blob_size);
    if (!blob)
    {
        printf("FATAL: cannot open data/dict.bin\n");
        return 1;
    }

    DictDB db = dict_open(blob);
    if (!db.hdr)
    {
        printf("FATAL: dict_open failed\n");
        bench_close_file(blob);
        return 1;
    }

    u32 total = db.hdr->word_count;
    printf("Dictionary: %u words\n\n", total);

    Arena scratch = arena_new(MB(1));

    typedef struct { const char* name; const char* text; } BenchQuery;
    BenchQuery queries[] = {
        {"(empty)",  ""},
        {"z",        "z"},
        {"a",        "a"},
        {"st",       "st"},
        {"pr",       "pr"},
        {"lnr",      "lnr"},
        {"the",      "the"},
        {"com",      "com"},
        {"abc",      "abc"},
        {"tes",      "tes"},
        {"tion",     "tion"},
        {"commu",    "commu"},
        {"linear",   "linear"},
        {"dict",     "dict"},
    };

    printf("%-10s %10s %10s %10s %9s\n", "Query", "Total(ms)", "Matched", "us/word", "Kw/s");
    printf("%s\n", "------------------------------------------------------");

    for (i32 qi = 0; qi < countof(queries); qi++)
    {
        String query = { (u8*)queries[qi].text, (isize)strlen(queries[qi].text) };

        LARGE_INTEGER freq, t0, t1;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);

        i32 matched = 0;
        for (u32 i = 0; i < total; i++)
        {
            const char* word = DICT_STR(&db, db.words[i].word_stroff);
            String candidate = { (u8*)word, (isize)strlen(word) };
            FuzzyMatch m = fuzzy_match(query, candidate, &scratch);
            if (m.score < 1e8f)
                matched++;
        }

        QueryPerformanceCounter(&t1);
        f64 ms = (f64)(t1.QuadPart - t0.QuadPart) * 1000.0 / (f64)freq.QuadPart;
        f64 us_per_word = ms * 1000.0 / (f64)total;
        f64 kw_per_s = ms > 0.0 ? (f64)total / ms : 0.0;

        printf("%-10s %10.2f %10d %10.1f %9.1f\n", queries[qi].name, ms, matched, us_per_word, kw_per_s);
    }

    arena_release(&scratch);
    bench_close_file(blob);

    printf("\n=== Bench complete ===\n");
    return 0;
}

#endif // FUZZY_BENCH
