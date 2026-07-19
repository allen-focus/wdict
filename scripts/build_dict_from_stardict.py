"""
build_dict_from_stardict.py — Convert data/stardict.db to our flat dictionary format.

Reads the StarDict SQLite database, takes the top TOP_N most-frequent words
(smallest `bnc` rank) and the top TOP_N most-frequent words (smallest `frq` rank),
unions the two sets, and extracts fields per word. `bnc`/`frq` are frequency
RANKS (1 = most common). The StarDict table has many rows per word (one per sense),
so we rank DISTINCT words and keep the single richest row per word.

    word, phonetic, definition, translation, exchange

The English `definition` field is intentionally stored EMPTY: it is never
displayed or searched at runtime (WORD mode shows phonetic + Chinese
translation; DEF search uses only the Chinese segment) and previously
accounted for ~67% of the uncompressed payload.

The frequency rank is used ONLY to decide which words to include — it is NOT
stored in the output. The `exchange` inflection string is stored verbatim.

Two artifacts are produced:
    data/dict.json   — flat debug dump: {word: {phonetic, definition, translation, exchange}}
    data/dict.bin    — our flat binary format (v4)

Binary layout (little-endian), reusing the DictFileHeader / DictWordIndex structs
so later C migration is trivial:

  [FileHeader    32 B fixed]
  [WordIndex[]   12 B per entry, sorted alphabetically (case-insensitive) by word]
  [EntryData     fixed 16 B per entry: phonetic_off, definition_off,
                  translation_off, exchange_off (all u32; 0 = empty string)]
  [StringPool    all UTF-8 strings null-terminated, offset 0 = empty string]

VariantIndex is absent (variant_off = 0, variant_count = 0) — exchange is stored
raw, not expanded into a variant map.

Usage:  python scripts/build_dict_from_stardict.py [stardict.db] [out_dir]
Defaults: data/stardict.db  ->  data/
"""

import json
import re
import struct
import sqlite3
import sys
from pathlib import Path

# ─── tuning ──────────────────────────────────────────────────────────────────

# Take the TOP_N most-frequent words (smallest bnc / frq rank) from each ranking,
# then union the two sets. bnc/frq are frequency RANKS (1 = most common); the
# StarDict table has many rows per word (one per sense), so we rank DISTINCT words
# and keep the top TOP_N of each, not a value threshold.
TOP_N = 20000

# ─── builder ──────────────────────────────────────────────────────────────────


class Builder:
    def __init__(self):
        # String pool: offset 0 is the empty string sentinel.
        self._strpool = bytearray(b"\x00")
        self._strcache = {"": 0}
        # EntryData: tightly packed 16-byte EntryBlobs.
        self._entdata = bytearray()
        # (word_lower, word_original, entdata_offset)
        self._words = []

    # ── string interning ──────────────────────────────────────────────────

    def _intern(self, s: str) -> int:
        """Return byte offset of null-terminated UTF-8 string in strpool."""
        if s in self._strcache:
            return self._strcache[s]
        off = len(self._strpool)
        self._strpool += s.encode("utf-8") + b"\x00"
        self._strcache[s] = off
        return off

    # ── low-level helpers ─────────────────────────────────────────────────

    @staticmethod
    def _u32(v: int) -> bytes:
        return struct.pack("<I", v)

    # ── helpers ─────────────────────────────────────────────────────────

    @staticmethod
    def _normalize_newlines(s: str) -> str:
        """Replace \r\n / \n with ；so the renderer sees a single-line string."""
        if not s:
            return s
        # Normalise Windows-style first, then Unix-style.
        s = s.replace("\r\n", "；")
        s = s.replace("\n", "；")
        return s

    # ── build entry ───────────────────────────────────────────────────────

    def _build_entry(
        self, phonetic: str, definition: str, translation: str, exchange: str
    ) -> int:
        """Build a flat 16-byte EntryBlob and append to entdata. Return its offset."""
        blob = bytearray()
        blob += self._u32(self._intern(phonetic))
        blob += self._u32(self._intern(definition))
        blob += self._u32(self._intern(translation))
        blob += self._u32(self._intern(exchange))
        off = len(self._entdata)
        self._entdata += blob
        return off

    # ── main entry point ──────────────────────────────────────────────────

    def build(self, db_path: str, out_dir: Path):
        con = sqlite3.connect(db_path)
        con.execute("PRAGMA query_only = ON")
        cur = con.cursor()

        # For each ranking, take the TOP_N DISTINCT words by smallest rank, then for
        # each such word keep the row with the richest definition text (longest
        # definition + translation). The two top-N word sets are unioned by word;
        # if a word appears in both, the bnc-picked row wins (fallback to frq).
        def collect(rank_col):
            # Top-N distinct words by this rank column. A rank value of 0 means
            # "unranked" (sentinel), so exclude it like NULL.
            cur.execute(
                f"""
                SELECT word FROM stardict
                WHERE {rank_col} IS NOT NULL AND {rank_col} > 0
                GROUP BY word
                ORDER BY MIN({rank_col}) ASC
                LIMIT ?
                """,
                (TOP_N,),
            )
            words = [r[0] for r in cur.fetchall()]
            if not words:
                return {}
            # Placeholder string for the IN list.
            ph = ",".join("?" * len(words))
            # Keep each word's frq rank for the frequency-weight signal: prefer the
            # real frq rank, falling back to bnc when frq is 0/NULL. 0 = unranked.
            cur.execute(
                f"""
                SELECT word, phonetic, definition, translation, exchange,
                       LENGTH(definition) + LENGTH(translation) AS richness,
                       COALESCE(NULLIF(frq, 0), NULLIF(bnc, 0)) AS freq_rank
                FROM stardict
                WHERE word IN ({ph})
                ORDER BY word ASC, richness DESC
                """,
                words,
            )
            best = {}
            for (
                word,
                phonetic,
                definition,
                translation,
                exchange,
                _rich,
                freq_rank,
            ) in cur:
                if word not in best:
                    best[word] = (
                        (phonetic, definition, translation, exchange),
                        int(freq_rank) if freq_rank is not None else None,
                    )
            return best

        bnc_words = collect("bnc")
        frq_words = collect("frq")

        # Union: bnc picks win; fill remaining from frq. Keep the frq rank for
        # each word (the value used by the runtime frequency-weight scorer).
        chosen = {}
        for word, val in frq_words.items():
            chosen[word] = val
        for word, val in bnc_words.items():
            if word not in chosen:
                chosen[word] = val

        # Sort alphabetically (case-insensitive) first, THEN append EntryBlobs so
        # that entdata_off values are monotonic and match the sorted WordIndex.
        items = []
        for word, (fields, rank) in chosen.items():
            phonetic, definition, translation, exchange = fields
            items.append(
                (
                    word.lower(),
                    word,
                    rank,
                    self._normalize_newlines(phonetic or ""),
                    # English `definition` is dropped: it is never displayed or
                    # searched at runtime (WORD mode shows phonetic + Chinese
                    # translation; DEF search uses only the Chinese segment) and
                    # accounted for ~67% of the uncompressed payload.
                    "",
                    self._normalize_newlines(translation or ""),
                    self._normalize_newlines(exchange or ""),
                )
            )
        items.sort(key=lambda x: x[0])

        for (
            word_lower,
            word,
            rank,
            phonetic,
            definition,
            translation,
            exchange,
        ) in items:
            ent_off = self._build_entry(phonetic, definition, translation, exchange)
            self._words.append((word_lower, word, rank, ent_off))
        con.close()

        # Phase: intern all word strings (after sort for cache locality).
        word_entries = []
        for _word_lower, word_original, rank, ent_off in self._words:
            word_entries.append((self._intern(word_original), ent_off, rank))

        # Assemble final blob.
        HDR_SIZE = 32
        WORDS_SIZE = len(word_entries) * 12

        words_off = HDR_SIZE
        entdata_off = words_off + WORDS_SIZE
        strpool_off = entdata_off + len(self._entdata)

        out = bytearray()
        out += struct.pack(
            "<IIIIIIII",
            0x44494354,  # magic "DICT"
            4,  # version (4 = flat 5-field format)
            len(word_entries),  # word_count
            words_off,
            entdata_off,
            strpool_off,
            0,  # variant_off (none)
            0,  # variant_count
        )

        # WordIndex[]
        for word_stroff, ent_off, rank in word_entries:
            # freq = frq rank (lower = more frequent); 0xFFFFFFFF sentinel if absent.
            freq = rank if rank is not None else 0xFFFFFFFF
            out += struct.pack("<III", word_stroff, ent_off, freq)

        # EntryData
        out += self._entdata

        # StringPool
        out += self._strpool

        # Write binary.
        bin_path = out_dir / "dict.bin"
        bin_path.write_bytes(out)

        # Write flat JSON debug dump (reconstructed from entdata + strpool).
        json_obj = self._build_json_dump()

        json_path = out_dir / "dict.json"
        json_path.write_text(json.dumps(json_obj, ensure_ascii=False), encoding="utf-8")

        # Stats.
        kib = 1024.0
        print(f"Source words (top {TOP_N} bnc \u222a top {TOP_N} frq): {len(chosen)}")
        print(f"Words:      {len(word_entries)}")
        print(f"Header:     {HDR_SIZE} B")
        print(f"WordIndex:  {WORDS_SIZE} B ({WORDS_SIZE / kib:.1f} KB)")
        print(
            f"EntryData:  {len(self._entdata):,} B ({len(self._entdata) / kib:.1f} KB)"
        )
        print(
            f"StringPool: {len(self._strpool):,} B ({len(self._strpool) / kib:.1f} KB)"
        )
        print(f"  (strings: {len(self._strcache)} unique)")
        print(f"Total:      {len(out):,} B ({len(out) / kib:.1f} KB)")
        print(f"Wrote:      {bin_path}")
        print(f"Wrote:      {json_path}")
        print(
            "Hint: compress with  python scripts/compress_dict.py data/dict.bin data/dict.bin.zstd"
        )

    def _build_json_dump(self) -> dict:
        """Reconstruct {word: {fields}} from entdata + strpool for the JSON dump.

        EntryBlob layout: 4 x u32 (phonetic, definition, translation, exchange).
        """
        result = {}
        sp = self._strpool
        base = 0
        for _word_lower, word_original, rank, ent_off in self._words:
            o = ent_off
            phonetic_off, definition_off, translation_off, exchange_off = (
                struct.unpack_from("<IIII", self._entdata, o)
            )
            result[word_original] = {
                "phonetic": self._str_at(sp, phonetic_off),
                "definition": self._str_at(sp, definition_off),
                "translation": self._str_at(sp, translation_off),
                "exchange": self._str_at(sp, exchange_off),
            }
            base += 1
        return result

    @staticmethod
    def _str_at(sp: bytearray, off: int) -> str:
        if off == 0:
            return ""
        end = sp.find(b"\x00", off)
        return sp[off:end].decode("utf-8", errors="replace")


# ─── CLI ─────────────────────────────────────────────────────────────────────


def main():
    db_path = sys.argv[1] if len(sys.argv) > 1 else "data/stardict.db"
    out_dir = Path(sys.argv[2]) if len(sys.argv) > 2 else Path("data")
    out_dir.mkdir(parents=True, exist_ok=True)
    Builder().build(db_path, out_dir)


if __name__ == "__main__":
    main()
