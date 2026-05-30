"""
build_dict.py — Convert data/dict.json to data/dict.bin

Binary layout (5 sections, little-endian):

  [FileHeader    32 B fixed]
  [WordIndex[]   12 B per entry, sorted alphabetically by word]
  [EntryData     variable-length EntryBlob per entry, tightly packed]
  [VariantIndex  8 B per entry, sorted alphabetically by variant word]
  [StringPool    all strings null-terminated, offset 0 = empty string]

Usage:  python scripts/build_dict.py data/dict.json data/dict.bin
"""

import json
import struct
import sys
from pathlib import Path

# ─── PosKind enumeration (matches C enum in dict.h) ──────────────────────────

POS_MAP = {
    "Noun":               0,
    "Verb":               1,
    "Noun, verb":         2,
    "Adjective":          3,
    "Adverb":             4,
    "Adjective, adverb":  5,
    "Conjunction":        6,
    "Determiner":         7,
    "Indefinite article": 8,
    "Interjection":       9,
    "Modal verb":         10,
    "Number":             11,
    "Predeterminer":      12,
    "Preposition":        13,
    "Adverb, preposition": 14,
    "Pronoun":            15,
    "Suffix":             16,
    "Prefix":             17,
    "Auxiliary verb":     18,
    "Phrasal verb":       19,
    "Definite article":   20,
}

POS_UNKNOWN = 0xFF
FREQ_NULL    = 0xFFFFFFFF

# ─── Builder ─────────────────────────────────────────────────────────────────

class Builder:
    def __init__(self):
        # String pool: offset 0 is the empty string sentinel.
        self._strpool = bytearray(b'\x00')
        self._strcache = {"": 0}

        # EntryData: tightly packed blob of all EntryBlobs.
        self._entdata = bytearray()

        # Accumulated (word_lower, word_original, entdata_offset)
        #   word_lower  — for case-insensitive sort
        #   word_original — the actual word string to intern
        self._words = []

        # Variant map: {variant_word -> base_word_original}
        self._variant_map = None

    # ── string interning ─────────────────────────────────────────────────

    def _intern(self, s: str) -> int:
        """Return byte offset of null-terminated UTF-8 string in strpool."""
        if s in self._strcache:
            return self._strcache[s]
        off = len(self._strpool)
        self._strpool += s.encode('utf-8') + b'\x00'
        self._strcache[s] = off
        return off

    # ── low-level helpers ────────────────────────────────────────────────

    @staticmethod
    def _u8(v: int) -> bytes:
        return struct.pack('<B', v)

    @staticmethod
    def _u32(v: int) -> bytes:
        return struct.pack('<I', v)

    # ── build sub-blocks ─────────────────────────────────────────────────

    def _build_brief_strings(self, items: list) -> bytes:
        """brief_en or brief_zh: u8 count + u32[] offsets into strpool."""
        buf = bytearray()
        buf += self._u8(len(items))
        for s in items:
            buf += self._u32(self._intern(str(s)))
        return bytes(buf)

    def _build_pos(self, pos_name: str, pos_data: dict) -> bytes:
        """Build a single PosBlob."""
        buf = bytearray()
        buf += self._u8(POS_MAP.get(pos_name, POS_UNKNOWN))
        buf += self._u32(self._intern(pos_data.get('pron', '')))
        defs = pos_data.get('def', [])
        buf += self._u8(len(defs))
        for d in defs:
            buf += self._u32(self._intern(d.get('en', '')))
            buf += self._u32(self._intern(d.get('zh', '')))
            exs = d.get('examples', [])
            buf += self._u8(len(exs))
            for ex in exs:
                buf += self._u32(self._intern(ex.get('en', '')))
                buf += self._u32(self._intern(ex.get('zh', '')))
        return bytes(buf)

    def _build_entry(self, entry: dict) -> int:
        """Build an EntryBlob and append to entdata.  Return its offset."""
        blob = bytearray()

        # freq (u32, 0xFFFFFFFF = null)
        freq_val = entry.get('freq')
        blob += self._u32(FREQ_NULL if freq_val is None else int(freq_val))

        # brief_en[] and brief_zh[]
        blob += self._build_brief_strings(entry.get('brief_en', []))
        blob += self._build_brief_strings(entry.get('brief_zh', []))

        # pos_node
        pos_nodes = entry.get('pos_node', {})
        blob += self._u8(len(pos_nodes))
        for pos_name, pos_data in pos_nodes.items():
            blob += self._build_pos(pos_name, pos_data)

        off = len(self._entdata)
        self._entdata += blob
        return off

    # ── main entry point ─────────────────────────────────────────────────

    def build(self, json_path: str, out_path: str):
        data = json.loads(Path(json_path).read_text(encoding='utf-8'))

        # Extract _variant_map before iterating so we don't try to build
        # an EntryBlob for it.
        self._variant_map = data.pop('_variant_map', None)

        # Phase 1: build all EntryBlobs, collect words.
        for word_original, entry in data.items():
            ent_off = self._build_entry(entry)
            self._words.append((word_original.lower(), word_original, ent_off))

        # Phase 2: sort alphabetically by word (case-insensitive).
        self._words.sort(key=lambda x: x[0])

        # Build a lookup from word_original -> sorted WordIndex position.
        word_index_of = {}
        for i, (_word_lower, word_original, _ent_off) in enumerate(self._words):
            word_index_of[word_original] = i

        # Build variant entries: (variant_word, base_word_index).
        variant_entries = []
        if self._variant_map:
            for variant_word, base_word in self._variant_map.items():
                base_idx = word_index_of.get(base_word)
                if base_idx is not None:
                    variant_entries.append((variant_word.lower(), self._intern(variant_word), base_idx))
            variant_entries.sort(key=lambda x: x[0])

        # Phase 3: intern all word strings (after sort so strpool order
        # matches the sorted WordIndex order, keeping word strings together
        # for better cache locality during search).
        word_entries = []
        for _word_lower, word_original, ent_off in self._words:
            # Look up freq from the original JSON entry to store in WordIndex.
            entry_dict = data[word_original]
            freq_val = entry_dict.get('freq')
            freq = FREQ_NULL if freq_val is None else int(freq_val)
            word_entries.append((self._intern(word_original), ent_off, freq))

        # Phase 4: compute section offsets and assemble final blob.
        HDR_SIZE       = 32
        WORDS_SIZE     = len(word_entries) * 12
        VARIANT_SIZE   = len(variant_entries) * 8

        words_off       = HDR_SIZE
        entdata_off     = words_off + WORDS_SIZE
        variant_off     = entdata_off + len(self._entdata) if VARIANT_SIZE > 0 else 0
        strpool_off     = (variant_off + VARIANT_SIZE) if VARIANT_SIZE > 0 else entdata_off + len(self._entdata)

        # ── Header ──
        out = bytearray()
        out += struct.pack('<IIIIIIII',
            0x44494354,               # magic "DICT"
            3,                        # version (3 = added VariantIndex)
            len(word_entries),        # word_count
            words_off,
            entdata_off,
            strpool_off,
            variant_off,              # → VariantIndex (0 = none)
            len(variant_entries))     # variant_count

        # ── WordIndex[] ──
        for word_stroff, ent_off, freq in word_entries:
            out += struct.pack('<III', word_stroff, ent_off, freq)

        # ── EntryData ──
        out += self._entdata

        # ── VariantIndex[] ──
        for _variant_lower, variant_stroff, base_idx in variant_entries:
            out += struct.pack('<II', variant_stroff, base_idx)

        # ── StringPool ──
        out += self._strpool

        # ── Write ──
        Path(out_path).write_bytes(out)

        # ── Stats ──
        kib = 1024.0
        print(f"Words:      {len(word_entries)}")
        print(f"Variants:   {len(variant_entries)}")
        print(f"Header:     {HDR_SIZE} B")
        print(f"WordIndex:  {WORDS_SIZE} B ({WORDS_SIZE/kib:.1f} KB)")
        print(f"EntryData:  {len(self._entdata):,} B ({len(self._entdata)/kib:.1f} KB)")
        print(f"VariantIdx: {VARIANT_SIZE} B ({VARIANT_SIZE/kib:.1f} KB)")
        print(f"StringPool: {len(self._strpool):,} B ({len(self._strpool)/kib:.1f} KB)")
        print(f"  (strings: {len(self._strcache)} unique)")
        print(f"Total:      {len(out):,} B ({len(out)/kib:.1f} KB)")

# ─── CLI ─────────────────────────────────────────────────────────────────────

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(f"Usage:  python {sys.argv[0]} <dict.json> <dict.bin>")
        sys.exit(1)
    Builder().build(sys.argv[1], sys.argv[2])
