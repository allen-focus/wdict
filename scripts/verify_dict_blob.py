"""Verify dict.bin structure by parsing it back and checking invariants."""
import struct, sys

if len(sys.argv) != 2:
    print(f"Usage:  python {sys.argv[0]} <dict.bin>")
    sys.exit(1)

with open(sys.argv[1], 'rb') as f:
    blob = f.read()

# ── Parse Header (v3: reserved[2] → variant_off, variant_count) ──
(magic, version, word_count, words_off,
 entdata_off, strpool_off, variant_off, variant_count) = struct.unpack_from('<IIIIIIII', blob, 0)

assert magic    == 0x44494354, f'bad magic: 0x{magic:08X}'
assert version in (2, 3),      f'bad version: {version}'
assert words_off == 32

if version == 2:
    assert variant_off == 0 and variant_count == 0, f'v2 reserved non-zero: {variant_off} {variant_count}'
    assert entdata_off == 32 + word_count * 12
    assert 0 < entdata_off <= strpool_off < len(blob), 'section misalignment'
else:
    # v3: sections are Header | WordIndex | EntryData | [VariantIndex] | StringPool
    assert 0 <= variant_off < len(blob)
    if variant_off == 0:
        assert variant_count == 0
        assert entdata_off == 32 + word_count * 12
        assert 0 < entdata_off <= strpool_off < len(blob), 'section misalignment'
    else:
        assert variant_count > 0
        assert variant_off >= entdata_off + 1  # at least past EntryData
        assert strpool_off == variant_off + variant_count * 8
        assert 0 < entdata_off <= variant_off < strpool_off < len(blob), 'section misalignment'

print(f'Header OK: {word_count} words, {variant_count} variants, {len(blob)} total bytes')

# ── Parse WordIndex ──
words = []
for i in range(word_count):
    off = words_off + i * 12
    ws, eo, fr = struct.unpack_from('<III', blob, off)
    end = blob.index(b'\x00', strpool_off + ws)
    word = blob[strpool_off + ws:end].decode('utf-8')
    words.append((word, ws, eo, fr))

# Check sorted alphabetically (case-insensitive).
for i in range(1, len(words)):
    a, b = words[i-1][0].lower(), words[i][0].lower()
    assert a <= b, f'not sorted at i={i}: "{words[i-1][0]}" > "{words[i][0]}"'
print('WordIndex OK: sorted alphabetically')

# Check no duplicate words.
seen = set()
for w, _, _, _ in words:
    assert w not in seen, f'duplicate word: {w}'
    seen.add(w)
print('WordIndex OK: no duplicates')

# Print first 5 entries.
print()
for w, ws, eo, fr in words[:5]:
    fr_s = 'null' if fr == 0xFFFFFFFF else str(fr)
    print(f'  {w:12s}  ws={ws:4d}  eo={eo:4d}  freq={fr_s}')

# ── Parse VariantIndex (v3 only) ──
if version >= 3 and variant_count > 0:
    variant_entries = []
    for i in range(variant_count):
        off = variant_off + i * 8
        vs, bi = struct.unpack_from('<II', blob, off)
        end = blob.index(b'\x00', strpool_off + vs)
        variant_word = blob[strpool_off + vs:end].decode('utf-8')
        assert 0 <= bi < word_count, f'variant[{i}] base_word_idx {bi} out of range'
        base_word = words[bi][0]
        variant_entries.append((variant_word, base_word, vs, bi))
    # Check sorted alphabetically (case-insensitive)
    for i in range(1, len(variant_entries)):
        a, b = variant_entries[i-1][0].lower(), variant_entries[i][0].lower()
        assert a <= b, f'variant index not sorted at i={i}: "{variant_entries[i-1][0]}" > "{variant_entries[i][0]}"'
    print(f'VariantIndex OK: {variant_count} entries, sorted')
    for vw, bw, vs, bi in variant_entries[:5]:
        print(f'  {vw:15s} → {bw:12s}  (idx={bi})')
else:
    print(f'\nVariantIndex: none (version={version})')

# ── EntryBlob reader helpers ──
def rd_u8(pos):
    v = blob[pos[0]]; pos[0] += 1; return v

def rd_u32(pos):
    v = struct.unpack_from('<I', blob, pos[0])[0]; pos[0] += 4; return v

def rd_str(off):
    if off == 0:
        return '(empty)'
    end = blob.index(b'\x00', strpool_off + off)
    return blob[strpool_off + off:end].decode('utf-8', errors='replace')

# ── Parse every EntryBlob and verify structural consistency ──
errors = 0
for wi, (word, _ws, eo, _fr) in enumerate(words):
    pos = [entdata_off + eo]
    try:
        # freq
        freq = rd_u32(pos)
        # brief_en
        n_be = rd_u8(pos)
        for _ in range(n_be):
            rd_u32(pos)  # offset
        # brief_zh
        n_bz = rd_u8(pos)
        for _ in range(n_bz):
            rd_u32(pos)
        # pos_node
        pos_count = rd_u8(pos)
        assert 0 < pos_count <= 32, f'bogus pos_count={pos_count}'
        for _ in range(pos_count):
            kind = rd_u8(pos)
            assert kind <= 255, f'bogus pos_kind={kind}'
            pron_off = rd_u32(pos)
            # verify pron string is valid
            if pron_off != 0:
                rd_str(pron_off)
            def_count = rd_u8(pos)
            assert def_count <= 255, f'bogus def_count={def_count}'
            for __ in range(def_count):
                en_off = rd_u32(pos)
                zh_off = rd_u32(pos)
                if en_off:
                    rd_str(en_off)
                if zh_off:
                    rd_str(zh_off)
                ex_count = rd_u8(pos)
                assert ex_count <= 255, f'bogus ex_count={ex_count}'
                for ___ in range(ex_count):
                    ex_en_off = rd_u32(pos)
                    ex_zh_off = rd_u32(pos)
                    if ex_en_off:
                        rd_str(ex_en_off)
                    if ex_zh_off:
                        rd_str(ex_zh_off)
        consumed = pos[0] - (entdata_off + eo)
        # Check that consumed bytes match distance to next entry (or end).
        if wi + 1 < len(words):
            next_eo = words[wi + 1][2]  # next entry's absolute offset in entdata
            expected = next_eo - eo     # byte count of this entry's blob
            assert consumed == expected, \
                f'{word}: consumed {consumed}B but expected {expected}B (next at {next_eo})'
        else:
            # Last entry: check we don't overshoot entdata.
            assert entdata_off + eo + consumed <= strpool_off
    except Exception as e:
        print(f'ERROR in {word}: {e}')
        errors += 1

# ── Parse first entry in detail ──
print(f'\n--- Detailed parse of first entry ---')
pos = [entdata_off + words[0][2]]
print(f'  word: {words[0][0]}')
print(f'  freq: {rd_u32(pos)}')
n_be = rd_u8(pos)
print(f'  brief_en ({n_be}):')
for i in range(n_be):
    print(f'    [{i}] {rd_str(rd_u32(pos))}')
n_bz = rd_u8(pos)
print(f'  brief_zh ({n_bz}):')
for i in range(n_bz):
    print(f'    [{i}] {rd_str(rd_u32(pos))}')
pc = rd_u8(pos)
print(f'  pos_count: {pc}')
for pi in range(min(pc, 2)):
    kind = rd_u8(pos)
    pron = rd_str(rd_u32(pos))
    dc = rd_u8(pos)
    print(f'    POS[{pi}] kind={kind} pron={pron} defs={dc}')
    for di in range(min(dc, 2)):
        en = rd_str(rd_u32(pos))
        zh = rd_str(rd_u32(pos))
        ec = rd_u8(pos)
        print(f'      def[{di}] en={en[:60]}...' if len(en)>60 else f'      def[{di}] en={en}')
        print(f'            zh={zh[:60]}...' if len(zh)>60 else f'            zh={zh}')
        for ei in range(min(ec, 1)):
            ex_en = rd_str(rd_u32(pos))
            ex_zh = rd_str(rd_u32(pos))
            print(f'        ex[{ei}] en={ex_en[:60]}...' if len(ex_en)>60 else f'        ex[{ei}] en={ex_en}')
            print(f'              zh={ex_zh[:60]}...' if len(ex_zh)>60 else f'              zh={ex_zh}')

# ── Summary ──
print(f'\n--- Result ---')
if errors:
    print(f'FAILED: {errors} entry parse errors')
    sys.exit(1)
else:
    print(f'All {word_count} entry blobs parsed successfully. Blob is valid!')
