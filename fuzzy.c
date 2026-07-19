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
#define FUZZY_EXACT_MATCH_BONUS   6.0f
#define FUZZY_PREFIX_BONUS        4.0f
#define FUZZY_LENGTH_PENALTY      0.5f
#define FUZZY_SPAN_PENALTY        0.3f

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

    /* Normalisation factor: log2(candidate length).  Used to scale position
       penalty and gap penalty so that matches in long texts (definition
       search for common polysemous words) are not dominated by candidate
       length.  Short candidates (word mode, 3‑20 chars) are virtually
       unaffected because log2 of small numbers is tiny. */
    f32 norm = log2f((f32)c_count + 1.0f);
    norm = norm < 1.0f ? 1.0f : norm;

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
            // Normalise position penalty by log2(candidate length) so that
            // matches in long definition texts (common polysemous words)
            // are not unfairly penalised relative to short definitions
            // (rare words).  Without this, a match at position 50 in a
            // 500‑char definition would be scored ~25× worse than a match
            // at position 2 in a 20‑char definition, even though both are
            // proportionally similar.  (See scripts/diagnose_def_search.py)
            f32 position_penalty = (f32)start / norm;
            f32 base = position_penalty;
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
                // Normalise gap penalty by the same log2 length factor
                // so that gaps are scored proportionally to text length.
                f32 adj = DP(q - 1, start) - (f32)(start + 1) * FUZZY_GAP_PENALTY / norm;
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
                        // Normalise gap penalty proportionally to text length.
                        f32 s = best_adj + (f32)c * FUZZY_GAP_PENALTY / norm;
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
                    f32 adj = DP(q - 1, c) - (f32)(c + 1) * FUZZY_GAP_PENALTY / norm;
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

    /* ---- post-processing: reward full/prefix match, penalise extra length & span ---- */
    if (best_score < FUZZY_DP_INF && best_count > 0)
    {
        // Exact match: every candidate char is matched
        if (best_count == c_count)
            best_score -= FUZZY_EXACT_MATCH_BONUS;

        // Normalise length/span penalties by the same log2 length factor
        // used for position and gap penalties (computed above, before the
        // DP loop).  See the comment there for rationale.

        // Longer candidates are worse
        best_score += (f32)(c_count - q_count) * FUZZY_LENGTH_PENALTY / norm;

        // Wider match span is worse (distance between first and last hit)
        i32 span = best_positions[best_count - 1] - best_positions[0] + 1;
        best_score += (f32)span * FUZZY_SPAN_PENALTY / norm;

        // Prefix match: consecutive from position 0
        if (best_positions[0] == 0 && span == best_count)
            best_score -= FUZZY_PREFIX_BONUS;
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
