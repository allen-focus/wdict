# wdict

## Features

- Tiny single-file executable (~1.2 MB) with no runtime dependencies and no data written to disk;
- Built-in English–Chinese dictionary, ready to use out of the box;
- `Ctrl+Alt+M` opens a fuzzy-search palette (e.g. "preent" → "present") with full keyboard navigation;
- Definition lookup mode to find words by meaning (e.g. "劳动" → "labor");
- `Ctrl+Alt` + left-click any text to look it up, showing a popup card;
- Automatically follows the system light/dark theme.

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

The blob uses the **flat v4 format** (see `build_dict_from_stardict.py`). Each entry stores four fields — phonetic, English definition, Chinese translation, and an inflection `exchange` string. Inflected forms are resolved at lookup time by scanning a word's `exchange` string (`"type:variant/..."`); there is no separate variant index section.

### Building the Blob

The dictionary is compiled from a StarDict SQLite database (`data/stardict.db`):

```
python scripts/build_dict_from_stardict.py data/stardict.db data/
```

This produces `data/dict.bin` (v4 flat format) and a `data/dict.json` flat debug dump. The English `definition` field is left empty on purpose — it is never displayed or searched at runtime.

Then compress it for embedding:

```
python scripts/compress_dict.py data/dict.bin data/dict.bin.zstd
```

The compressed binary reduces the `.exe` size by ≈67%. You can validate a compiled blob with:

```
python scripts/verify_dict_blob.py data/dict.bin
```

The blob is little-endian and consists of four contiguous sections:

```
┌─────────────────────┐  offset 0
│  FileHeader  32 B   │
├─────────────────────┤  header.words_off
│  WordIndex[]        │  12 bytes per entry, sorted alphabetically
├─────────────────────┤  header.entdata_off
│  EntryData          │  fixed 16 B per entry (see EntryBlob below)
├─────────────────────┤  header.strpool_off
│  StringPool         │  all strings null-terminated, offset 0 = empty string
└─────────────────────┘
```

### FileHeader (32 bytes, packed)

| Offset   | Field            | Type   | Description                              |
| -------- | ---------------- | ------ | ---------------------------------------- |
| 0        | `magic`          | u32    | `0x44494354` ("DICT")                    |
| 4        | `version`        | u32    | `4` (flat 5-field format)                |
| 8        | `word_count`     | u32    | Number of word entries                   |
| 12       | `words_off`      | u32    | Byte offset to WordIndex[]               |
| 16       | `entdata_off`    | u32    | Byte offset to EntryData                 |
| 20       | `strpool_off`    | u32    | Byte offset to StringPool                |
| 24       | `variant_off`    | u32    | Byte offset to VariantIndex[] (0 = none) |
| 28       | `variant_count`  | u32    | Number of DictVariantEntry entries (0)   |

### WordIndex (12 bytes per entry, sorted alphabetically by word)

| Offset   | Field         | Type   | Description                                               |
| -------- | ------------- | ------ | --------------------------------------------------------- |
| 0        | `word_stroff` | u32    | Offset into StringPool (null-terminated word string)      |
| 4        | `entdata_off` | u32    | Offset into EntryData (start of this entry's EntryBlob)   |
| 8        | `freq`        | u32    | Frequency rank (lower = more common); `0xFFFFFFFF` = null |

### EntryData — EntryBlob layout (fixed 16 bytes per entry)

Each EntryBlob is four `u32` string-pool offsets, in this order:

| Offset | Field             | Type | Description                                                      |
| ------ | ----------------- | ---- | ---------------------------------------------------------------- |
| 0      | `phonetic_off`    | u32  | Offset into StringPool (IPA pronunciation; 0 = none)             |
| 4      | `def_off`         | u32  | Offset into StringPool (English definition; 0 = none)            |
| 8      | `translation_off` | u32  | Offset into StringPool (Chinese translation; 0 = none)           |
| 12     | `exchange_off`    | u32  | Offset into StringPool (inflection `type:variant/...`; 0 = none) |

`dict_resolve()` scans a word's `exchange` string to map inflected forms (e.g. "apples") back to their headword ("apple").

### StringPool

All strings are concatenated as UTF-8, each terminated with `\0`. Offset 0 is the empty string (a lone `\0`). Field values of `0` therefore denote "absent" or "empty".

## License

**Project code**: Released under the Unlicense (public domain).

**Dependencies**

- [Zstandard](https://github.com/facebook/zstd) (trimmed to `common/`, `decompress/`, `zstd.h`, `zstd_errors.h`, `LICENSE` only, version 1.5.7): Licensed under the BSD 3-Clause License. See the full terms in `thirdparty/zstd/LICENSE`.
- [Tracy](https://github.com/wolfpld/tracy) (trimmed to `public/` only, version 0.13.1): Licensed under the BSD 3‑Clause License. See the full terms in `thirdparty/tracy/LICENSE`.
- Icon fonts: Generated using [Fontello](https://fontello.com/). Contains icons from:
  - MFG Labs: Licensed under the [SIL Open Font License](http://scripts.sil.org/OFL). See the full terms in `thirdparty/fontello/LICENSE.txt`.
- [ECDICT](https://github.com/skywind3000/ECDICT) (trimmed and compressed dictionary data, generated via `scripts/build_dict_from_stardict.py` and `scripts/compress_dict.py`): Licensed under the MIT License. See the full terms in `data/LICENSE`.
