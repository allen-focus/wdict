## Build

To build the binary from source code, have [Visual Studio][VS] installed first, then you can use either of the following methods:

**Option 1**: build script

Simply run `build.bat`. The following commands are supported:

- `build` – equivalent to `build release`
- `build release`
- `build debug`

**Option 2**: CMake with Ninja (recommended for advanced configuration)

> NOTE: Unlike the batch script, CMake does not automatically initialize the MSVC compilation environment (e.g. `cl.exe`). You need to ensure that the MSVC compiler environment is available in your current session before running CMake.

```
cmake -B build -G "Ninja Multi-Config"
cmake --build build --config release
```

`--config` can be `release`, `debug`, or `profile`.


## Dictionary Data

The application embeds an English–Chinese dictionary as a compressed zstd binary blob (`data/dict.bin.zstd`) via `resource.rc` (`DICT_DATA RCDATA`). At startup, `startup_dict_thread` decompresses it (≈70 ms) into an arena-allocated buffer, then `DictDB` (see `dict.h`) opens it with zero-copy parsing — all pointers point directly into the decompressed memory, requiring no additional heap allocation.

When clicking an inflected word within a definition/example (e.g. "apples", "abandoned"), `dict_resolve()` first looks up the word directly; if not found, it falls back to binary search over a `VariantIndex` section to find the headword.

To compress the dictionary after building:
```
python scripts/compress_dict.py data/dict.bin data/dict.bin.zstd
```

The compressed binary reduces the `.exe` size by ≈67%.

The blob is little-endian and consists of five contiguous sections (v3+):

```
┌─────────────────────┐  offset 0
│  FileHeader  32 B   │
├─────────────────────┤  header.words_off
│  WordIndex[]        │  12 bytes per entry, sorted alphabetically
├─────────────────────┤  header.entdata_off
│  EntryData          │  variable-length EntryBlob per entry, tightly packed
├─────────────────────┤  header.variant_off (v3+)
│  VariantIndex[]     │  8 bytes per entry, sorted alphabetically (absent in v2)
├─────────────────────┤  header.strpool_off
│  StringPool         │  all strings null-terminated, offset 0 = empty string
└─────────────────────┘
```

### FileHeader (32 bytes, packed)

| Offset   | Field            | Type   | Description                              |
| -------- | ---------------- | ------ | ---------------------------------------- |
| 0        | `magic`          | u32    | `0x44494354` ("DICT")                    |
| 4        | `version`        | u32    | `3` (v2 → v3: added VariantIndex)        |
| 8        | `word_count`     | u32    | Number of word entries                   |
| 12       | `words_off`      | u32    | Byte offset to WordIndex[]               |
| 16       | `entdata_off`    | u32    | Byte offset to EntryData                 |
| 20       | `strpool_off`    | u32    | Byte offset to StringPool                |
| 24       | `variant_off`    | u32    | Byte offset to VariantIndex[] (0 = none) |
| 28       | `variant_count`  | u32    | Number of DictVariantEntry entries       |

### VariantIndex (8 bytes per entry, sorted alphabetically by variant word)

| Offset | Field            | Type | Description                                           |
| ------ | ---------------  | ---- | ----------------------------------------------------- |
| 0      | `variant_stroff` | u32  | Offset into StringPool (null-terminated variant word) |
| 4      | `base_word_idx`  | u32  | Index into WordIndex[] for the headword entry         |

`dict_resolve()` performs a case-insensitive binary search on this array when `dict_lookup()` fails, enabling inflected form (e.g. "apples") to headword ("apple") resolution.

### WordIndex (12 bytes per entry, sorted alphabetically by word)

| Offset   | Field         | Type   | Description                                               |
| -------- | ------------- | ------ | --------------------------------------------------------- |
| 0        | `word_stroff` | u32    | Offset into StringPool (null-terminated word string)      |
| 4        | `entdata_off` | u32    | Offset into EntryData (start of this entry's EntryBlob)   |
| 8        | `freq`        | u32    | Frequency rank (lower = more common); `0xFFFFFFFF` = null |

### EntryData — EntryBlob layout (variable-length, sequential read)

Each EntryBlob is parsed byte-by-byte in the following order:

```
u32 freq                       // 0xFFFFFFFF = null
u8  brief_en_count
u32 brief_en[brief_en_count]   // each: strpool offset
u8  brief_zh_count
u32 brief_zh[brief_zh_count]   // each: strpool offset
u8  pos_count

For each POS (0 … pos_count-1):
  u8  pos_kind                 // PosKind enum (0=Noun, 1=Verb, …, 20=Definite article, 255=Unknown)
  u32 pron_off                 // strpool offset (0 = none)
  u8  def_count

  For each definition (0 … def_count-1):
    u32 en_off                 // strpool offset — English definition
    u32 zh_off                 // strpool offset — Chinese definition
    u8  ex_count

    For each example (0 … ex_count-1):
      u32 en_off               // strpool offset — English example sentence
      u32 zh_off               // strpool offset — Chinese translation
```

### PosKind enumeration

| Value   | Part of Speech      |
| ------- | ------------------- |
| 0       | Noun                |
| 1       | Verb                |
| 2       | Noun, verb          |
| 3       | Adjective           |
| 4       | Adverb              |
| 5       | Adjective, adverb   |
| 6       | Conjunction         |
| 7       | Determiner          |
| 8       | Indefinite article  |
| 9       | Interjection        |
| 10      | Modal verb          |
| 11      | Number              |
| 12      | Predeterminer       |
| 13      | Preposition         |
| 14      | Adverb, preposition |
| 15      | Pronoun             |
| 16      | Suffix              |
| 17      | Prefix              |
| 18      | Auxiliary verb      |
| 19      | Phrasal verb        |
| 20      | Definite article    |
| 255     | Unknown             |

### StringPool

All strings are concatenated as UTF-8, each terminated with `\0`. Offset 0 is the empty string (a lone `\0`). Field values of `0` therefore denote "absent" or "empty".

## Building the Blob from JSON

A Python helper script is provided to convert a JSON dictionary file into the binary blob:

```
python scripts/build_dict.py <input.json> <output.bin>
```

### Expected JSON format

The top-level object maps each headword to its entry. An optional `_variant_map` key provides the reverse mapping from inflected form to headword — `build_dict.py` skips it as a word entry and builds the `VariantIndex` binary section from it instead.

```jsonc
{
  "aardvark": {
    "word": "aardvark",
    "freq": 42238,                  // u32 or null
    "brief_en": [                   // array of short English glosses
      "n. nocturnal burrowing mammal …"
    ],
    "brief_zh": [                   // array of short Chinese glosses
      "n. 土豚"
    ],
    "pos_node": {
      "Noun": {                     // part-of-speech name → PosKind enum
        "pron": "ˈɑːdvɑːk",         // pronunciation (IPA), may be ""
        "def": [                    // definitions for this POS
          {
            "en": "a large animal …",
            "zh": "土豚〔非洲南部 …〕",
            "examples": [           // may be []
              {
                "en": "…",
                "zh": "…"
              }
            ]
          }
        ]
      }
    }
  }
}
```

The optional `_variant_map` section maps inflected forms to their headwords:

```jsonc
{
  "apple": {
    "word": "apple",
    "freq": 42238,
    "brief_en": ["..."],
    "brief_zh": ["..."],
    "pos_node": { ... }
  },
  "_variant_map": {
    "apples": "apple",
    "abandoned": "abandon",
    "abandoning": "abandon",
    "abandons": "abandon",
    "better": "good",
    "running": "run",
    "sang": "sing",
    ...
  }
}
```

Field notes:

- `freq` — `null` if unknown. Used by `dict_fuzzy_search()` to boost common words.
- `brief_en` / `brief_zh` — short glosses for quick-lookup UI. May be empty arrays.
- `pron` — IPA pronunciation string. Empty string if absent.
- `pos_node` keys — matched to `PosKind` by name (case-sensitive). Unrecognised names become `POS_UNKNOWN` (255).
- `_variant_map` — `build_dict.py` skips this key and builds the `VariantIndex` binary section from it for runtime `dict_resolve()` lookups.


## License

**Project code**: Released under the Unlicense (public domain).

**Dependencies**

- [Zstandard](https://github.com/facebook/zstd) (trimmed to `common/`, `decompress/`, `zstd.h`, `zstd_errors.h`, `LICENSE` only, version 1.5.7): Licensed under the BSD 3-Clause License. See the full terms in `thirdparty/zstd/LICENSE`.
- [Tracy](https://github.com/wolfpld/tracy) (trimmed to `public/` only, version 0.13.1): Licensed under the BSD 3‑Clause License. See the full terms in `thirdparty/tracy/LICENSE`.
- Icon fonts: Generated using [Fontello](https://fontello.com/). Contains icons from:
  - MFG Labs: Licensed under the [SIL Open Font License](http://scripts.sil.org/OFL). See the full terms in `thirdparty/fontello/LICENSE.txt`.
