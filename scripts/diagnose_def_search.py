"""
diagnose_def_search.py — Replicate the exact fuzzy_match scoring pipeline
from fuzzy.c + search.c + dict.c to diagnose why definition-based reverse
lookup (e.g. "跑步") ranks common words like "run" behind rare words like
"lope".

Usage:  python scripts/diagnose_def_search.py data/dict.bin

This script mirrors:
  - fuzzy.c:   fuzzy_match()  (codepoint-level DP with identical scoring)
  - dict.c:    dict_build_search_aux() entry parsing
  - main.c:    dict_freq_weight() score adjustment
"""

import struct
import math
import sys
from pathlib import Path

# ─── Constants (mirrors fuzzy.h / dict.h) ─────────────────────────────────────

FUZZY_GAP_PENALTY         = 2.0
FUZZY_CONSECUTIVE_BONUS   = 3.0
FUZZY_WORD_BOUNDARY_BONUS = 2.0
FUZZY_CAMEL_BONUS         = 1.0
FUZZY_FIRST_CHAR_BONUS    = 2.0
FUZZY_EXACT_MATCH_BONUS   = 6.0
FUZZY_PREFIX_BONUS        = 4.0
FUZZY_LENGTH_PENALTY      = 0.5
FUZZY_SPAN_PENALTY        = 0.3
FUZZY_DP_INF              = 1e9

DICT_MAGIC                = 0x44494354
DICT_SEARCH_SEP           = 0x01

FREQ_NULL                 = 0xFFFFFFFF
FREQ_WEIGHT               = 0.3  # from dict_freq_weight in main.c

# ─── UTF-8 helpers ────────────────────────────────────────────────────────────

def utf8_decode_first(data, offset):
    """Decode the first UTF-8 codepoint starting at byte offset.
    Returns (codepoint, num_bytes)."""
    b0 = data[offset]
    if b0 < 0x80:
        return b0, 1
    elif b0 < 0xE0:
        return ((b0 & 0x1F) << 6) | (data[offset+1] & 0x3F), 2
    elif b0 < 0xF0:
        return ((b0 & 0x0F) << 12) | ((data[offset+1] & 0x3F) << 6) | (data[offset+2] & 0x3F), 3
    else:
        return ((b0 & 0x07) << 18) | ((data[offset+1] & 0x3F) << 12) | ((data[offset+2] & 0x3F) << 6) | (data[offset+3] & 0x3F), 4

def to_lower_cp(cp):
    if 0x41 <= cp <= 0x5A:  # A-Z
        return cp + 0x20
    return cp

def is_upper_cp(cp):
    return 0x41 <= cp <= 0x5A

def is_cjk_cp(cp):
    return ((0x4E00 <= cp <= 0x9FFF) or
            (0x3400 <= cp <= 0x4DBF) or
            (0xF900 <= cp <= 0xFAFF))

def is_alnum_cp(cp):
    return ((0x61 <= cp <= 0x7A) or (0x41 <= cp <= 0x5A) or (0x30 <= cp <= 0x39))

def prev_char_start(data, byte_offset):
    """Walk backward from byte_offset to find the start byte of the
    containing UTF-8 character."""
    off = byte_offset - 1
    while off > 0 and (data[off] & 0xC0) == 0x80:
        off -= 1
    return off

def is_word_start(data, length, byte_offset):
    """Mirrors fuzzy.c:is_word_start()."""
    if byte_offset == 0:
        return True
    if byte_offset >= length:
        return False

    prev_start = prev_char_start(data, byte_offset)
    prev_cp, _ = utf8_decode_first(data, prev_start)
    cur_cp, _ = utf8_decode_first(data, byte_offset)

    if is_cjk_cp(cur_cp):
        return True
    if is_cjk_cp(prev_cp):
        return True

    if prev_cp in (0x20, 0x2D, 0x5F, 0x2F, 0x5C, 0x2E, 0x2C, 0x28, 0x5B, 0x7B, 0x3A):
        return True  # space - _ / \ . , ( [ { :

    if is_upper_cp(cur_cp) and not is_upper_cp(to_lower_cp(prev_cp)) and is_alnum_cp(prev_cp):
        return True  # camelCase

    if is_alnum_cp(cur_cp) and not is_alnum_cp(prev_cp):
        return True  # non-alnum -> alnum

    return False

# ─── String decode to CharInfo array ──────────────────────────────────────────

class CharInfo:
    __slots__ = ('byte_offset', 'byte_len', 'cp', 'is_word_start')
    def __init__(self, byte_offset, byte_len, cp, is_word_start):
        self.byte_offset = byte_offset
        self.byte_len = byte_len
        self.cp = cp
        self.is_word_start = is_word_start

def decode_string(data, length, want_word_start):
    """Mirrors fuzzy.c:decode_string()."""
    result = []
    i = 0
    while i < length:
        cp, blen = utf8_decode_first(data, i)
        result.append(CharInfo(
            byte_offset=i,
            byte_len=blen,
            cp=to_lower_cp(cp),
            is_word_start=is_word_start(data, length, i) if want_word_start else False
        ))
        i += blen
    return result

# ─── Fuzzy match (mirrors fuzzy.c:fuzzy_match exactly) ────────────────────────

def fuzzy_match(query_str, candidate_str):
    """Mirrors fuzzy.c:fuzzy_match().  Returns (score, range_count, ranges).
    score >= 1e8 means no match.  Lower (including negative) = better."""
    if not query_str:
        return 0.0, 0, []
    if not candidate_str:
        return FUZZY_DP_INF, 0, []

    q_data = query_str.encode('utf-8')
    c_data = candidate_str.encode('utf-8')

    q_chars = decode_string(q_data, len(q_data), False)
    c_chars = decode_string(c_data, len(c_data), True)
    q_count = len(q_chars)
    c_count = len(c_chars)

    if q_count == 0:
        return 0.0, 0, []
    if c_count < q_count:
        return FUZZY_DP_INF, 0, []

    # DP tables: dp[q][c], parent[q][c]
    dp = [[FUZZY_DP_INF] * c_count for _ in range(q_count)]
    parent = [[-1] * c_count for _ in range(q_count)]

    best_score = FUZZY_DP_INF
    best_positions = [0] * q_count
    best_count = 0

    first_q_cp = q_chars[0].cp

    # ---- per-start DP ----
    for start in range(c_count - q_count + 1):
        if c_chars[start].cp != first_q_cp:
            continue

        # Clear rows
        for q in range(q_count):
            for c in range(start, c_count):
                dp[q][c] = FUZZY_DP_INF

        # Base case: query[0] at candidate[start]
        base = float(start)  # position penalty
        if start == 0:
            base -= FUZZY_FIRST_CHAR_BONUS
        if c_chars[start].is_word_start:
            base -= FUZZY_WORD_BOUNDARY_BONUS
        orig_cp, _ = utf8_decode_first(c_data, c_chars[start].byte_offset)
        if is_upper_cp(orig_cp):
            base -= FUZZY_CAMEL_BONUS
        dp[0][start] = base
        parent[0][start] = -1

        # Fill DP row by row
        for q in range(1, q_count):
            target_cp = q_chars[q].cp

            best_adj = FUZZY_DP_INF
            best_adj_prev = -1

            # Pre-seed non-adjacent pool with base-case position
            if dp[q-1][start] < FUZZY_DP_INF:
                best_adj = dp[q-1][start] - float(start + 1) * FUZZY_GAP_PENALTY
                best_adj_prev = start

            for c in range(start + 1, c_count):
                if c >= q and c_chars[c].cp == target_cp:
                    # Adjacent transition
                    prev = c - 1
                    if dp[q-1][prev] < FUZZY_DP_INF:
                        s = dp[q-1][prev] - FUZZY_CONSECUTIVE_BONUS
                        if c_chars[c].is_word_start:
                            s -= FUZZY_WORD_BOUNDARY_BONUS
                        orig_cp2, _ = utf8_decode_first(c_data, c_chars[c].byte_offset)
                        if is_upper_cp(orig_cp2):
                            s -= FUZZY_CAMEL_BONUS
                        if s < dp[q][c]:
                            dp[q][c] = s
                            parent[q][c] = prev

                    # Non-adjacent transition
                    if best_adj < FUZZY_DP_INF:
                        s = best_adj + float(c) * FUZZY_GAP_PENALTY
                        if c_chars[c].is_word_start:
                            s -= FUZZY_WORD_BOUNDARY_BONUS
                        orig_cp2, _ = utf8_decode_first(c_data, c_chars[c].byte_offset)
                        if is_upper_cp(orig_cp2):
                            s -= FUZZY_CAMEL_BONUS
                        if s < dp[q][c]:
                            dp[q][c] = s
                            parent[q][c] = best_adj_prev

                # Add candidate position c to non-adjacent pool
                if dp[q-1][c] < FUZZY_DP_INF:
                    adj = dp[q-1][c] - float(c + 1) * FUZZY_GAP_PENALTY
                    if adj < best_adj:
                        best_adj = adj
                        best_adj_prev = c

        # Pick best end position for this start
        for c in range(start + q_count - 1, c_count):
            if dp[q_count - 1][c] < best_score:
                best_score = dp[q_count - 1][c]
                best_count = q_count
                idx = c
                for qi in range(q_count - 1, -1, -1):
                    best_positions[qi] = idx
                    idx = parent[qi][idx]

    # ---- Post-processing ----
    if best_score < FUZZY_DP_INF and best_count > 0:
        if best_count == c_count:
            best_score -= FUZZY_EXACT_MATCH_BONUS

        # Normalise length/span penalties by log2(candidate length).
        # (Matches the fix applied to fuzzy.c)
        norm = math.log2(float(c_count) + 1.0)
        norm = norm if norm >= 1.0 else 1.0

        best_score += float(c_count - q_count) * FUZZY_LENGTH_PENALTY / norm

        span = best_positions[best_count - 1] - best_positions[0] + 1
        best_score += float(span) * FUZZY_SPAN_PENALTY / norm

        if best_positions[0] == 0 and span == best_count:
            best_score -= FUZZY_PREFIX_BONUS

    if best_score >= 1e8:
        return FUZZY_DP_INF, 0, []

    # Build match ranges
    ranges = []
    rstart = c_chars[best_positions[0]].byte_offset
    rend = rstart + c_chars[best_positions[0]].byte_len

    for i in range(1, best_count):
        ci = c_chars[best_positions[i]]
        if ci.byte_offset == rend:
            rend = ci.byte_offset + ci.byte_len
        else:
            ranges.append((rstart, rend))
            rstart = ci.byte_offset
            rend = ci.byte_offset + ci.byte_len
    ranges.append((rstart, rend))

    return best_score, len(ranges), ranges

# ─── dict_freq_weight (mirrors main.c:dict_freq_weight) ───────────────────────

def dict_freq_weight(freq, raw_score):
    """Mirrors main.c:dict_freq_weight()."""
    if freq == FREQ_NULL:
        freq_score = 5.0
    else:
        freq_score = math.log2(1.0 + float(freq)) * 0.8
    return raw_score * (1.0 - FREQ_WEIGHT) + freq_score * FREQ_WEIGHT

# ─── Dict binary parser ───────────────────────────────────────────────────────

class DictReader:
    """Read v4 dict.bin and provide lookup + search_aux functionality."""

    def __init__(self, path):
        self.data = Path(path).read_bytes()

        # Header (32 bytes, little-endian)
        magic, version, wc, words_off, entdata_off, strpool_off, variant_off, variant_count = \
            struct.unpack_from('<IIIIIIII', self.data, 0)

        assert magic == DICT_MAGIC, f"Bad magic: 0x{magic:08X}"
        assert 2 <= version <= 4, f"Unsupported version: {version}"

        self.word_count = wc
        self.words_off = words_off
        self.entdata_off = entdata_off
        self.strpool_off = strpool_off
        self.variant_off = variant_off
        self.variant_count = variant_count

    def str_at(self, offset):
        """Read null-terminated UTF-8 string from strpool at offset."""
        if offset == 0:
            return ""
        end = self.data.index(0, self.strpool_off + offset)
        return self.data[self.strpool_off + offset:end].decode('utf-8', errors='replace')

    def get_word_index(self, idx):
        """Read DictWordIndex at given index."""
        off = self.words_off + idx * 12
        word_stroff, entdata_off, freq = struct.unpack_from('<III', self.data, off)
        return word_stroff, entdata_off, freq

    def find_word(self, word):
        """Case-insensitive binary search; returns (idx, word_stroff, entdata_off, freq) or None."""
        target = word.lower()
        lo, hi = 0, self.word_count - 1
        while lo <= hi:
            mid = (lo + hi) // 2
            ws, eo, freq = self.get_word_index(mid)
            mid_word = self.str_at(ws).lower()
            if mid_word == target:
                return mid, ws, eo, freq
            elif mid_word < target:
                lo = mid + 1
            else:
                hi = mid - 1
        return None

    def search_aux_for(self, idx):
        """Build DictSearchAuxEntry for a single word at WordIndex[idx].
        Returns (def_search_text, def_len, [(seg_offset, seg_len, seg_kind)])."""
        _, ent_off, _ = self.get_word_index(idx)
        p = self.entdata_off + ent_off

        # v4 EntryBlob: 4 x u32 offsets
        phonetic_off, def_off, tr_off, exchange_off = \
            struct.unpack_from('<IIII', self.data, p)

        result_text = ""
        segs = []
        cur_off = 0

        # Segment 0: English definition
        if def_off:
            s = self.str_at(def_off)
            if s:
                result_text += s + '\x01'
                segs.append((cur_off, len(s), 'def_en'))
                cur_off += len(s) + 1

        # Segment 1: Chinese translation
        if tr_off:
            s = self.str_at(tr_off)
            if s:
                result_text += s + '\x01'
                segs.append((cur_off, len(s), 'def_zh'))
                cur_off += len(s) + 1

        return result_text, cur_off, segs


# ─── Main ─────────────────────────────────────────────────────────────────────

def main():
    if len(sys.argv) < 2:
        bin_path = 'data/dict.bin'
    else:
        bin_path = sys.argv[1]

    db = DictReader(bin_path)
    print(f"Dictionary: {db.word_count} words")

    # Words to investigate
    words_of_interest = ['run', 'lope', 'trotter', 'runner', 'jog', 'sprint']

    # Also find ALL words whose Chinese def contains "跑步"
    query = "跑步"
    print(f"\n{'='*80}")
    print(f"Fuzzy match scores: query='{query}' against definition search text")
    print(f"Algorithm: fuzzy.c DP + length/span normalization + dict_freq_weight")
    print(f"{'='*80}")
    print(f"{'Word':<16} {'freq_rank':>10} {'c_chars':>8} {'raw_score':>12} {'freq_score':>10} {'adjusted':>12} {'segments':>20}")
    print(f"{'-'*16} {'-'*10} {'-'*8} {'-'*12} {'-'*10} {'-'*12} {'-'*20}")

    for w in words_of_interest:
        result = db.find_word(w)
        if result is None:
            print(f"{w:<16}  NOT FOUND")
            continue

        idx, ws, eo, freq = result
        def_text, def_len, segs = db.search_aux_for(idx)

        raw_score, range_cnt, ranges = fuzzy_match(query, def_text)

        if raw_score >= FUZZY_DP_INF:
            print(f"{w:<16} {str(freq) if freq != FREQ_NULL else 'NULL':>10} {'N/A':>8} {'NO MATCH':>12}")
            continue

        c_count = len(def_text.encode('utf-8'))
        freq_score_val = 5.0 if freq == FREQ_NULL else math.log2(1.0 + float(freq)) * 0.8
        adjusted = dict_freq_weight(freq, raw_score)

        seg_desc = ", ".join(f"{k}:{l}ch" for off, l, k in segs)

        print(f"{w:<16} {str(freq) if freq != FREQ_NULL else 'NULL':>10} {c_count:>8} {raw_score:>12.2f} {freq_score_val:>10.2f} {adjusted:>12.2f} {seg_desc:>20}")

        # Show the definition text segments
        for off, slen, kind in segs:
            snippet = def_text[off:off+slen]
            if len(snippet) > 80:
                snippet = snippet[:77] + "..."
            print(f"             [{kind}]  {snippet}")

        if ranges:
            # Show what got matched
            matched_parts = []
            for rs, re in ranges:
                # clamp to valid range
                rs = max(0, min(rs, len(def_text)))
                re = max(0, min(re, len(def_text)))
                matched_parts.append(repr(def_text[rs:re]))
            print(f"             MATCHED:  {', '.join(matched_parts)}")

    # ── Now scan ALL words for this query ──
    print(f"\n{'='*80}")
    print(f"Full scan: top 20 words matching '{query}' (by raw fuzzy score)")
    print(f"{'='*80}")

    all_results = []
    scan_count = min(db.word_count, 10000)  # limit for speed

    for idx in range(scan_count):
        def_text, def_len, segs = db.search_aux_for(idx)
        raw_score, range_cnt, ranges = fuzzy_match(query, def_text)
        if raw_score < FUZZY_DP_INF:
            ws, eo, freq = db.get_word_index(idx)
            word_str = db.str_at(ws)
            adjusted = dict_freq_weight(freq, raw_score)
            c_count = len(def_text.encode('utf-8'))
            all_results.append((adjusted, word_str, freq, raw_score, c_count))

    all_results.sort(key=lambda x: x[0])  # sort by adjusted score (lower = better)

    print(f"{'Rank':<5} {'Word':<16} {'freq':>10} {'c_chars':>8} {'raw':>12} {'adjusted':>12}")
    print(f"{'-'*5} {'-'*16} {'-'*10} {'-'*8} {'-'*12} {'-'*12}")
    for i, (adj, word, freq, raw, clen) in enumerate(all_results[:20]):
        freq_str = str(freq) if freq != FREQ_NULL else 'NULL'
        print(f"{i+1:<5} {word:<16} {freq_str:>10} {clen:>8} {raw:>12.2f} {adj:>12.2f}")


if __name__ == '__main__':
    main()
