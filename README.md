# Build

To build the binary from source code, have [Visual Studio][VS] installed first, then you can use either of the following methods:

**Option 1**: build script

Simply run `build.bat`. The following commands are supported:

- `build` – equivalent to `build release`
- `build release`
- `build debug`

**Option 2**: CMake with Ninja (recommended for advanced configuration)

```
cmake -B build -G "Ninja Multi-Config"
cmake --build build --config release
```

`--config` can be `release`, `debug`, or `profile`.

# License

**Project code**: Released under the Unlicense (public domain).

**Dependencies**

- [Tracy](https://github.com/wolfpld/tracy) (trimmed to `public/` only, version 0.13.1): Licensed under the BSD 3‑Clause License. See the full terms in `third_party/tracy/LICENSE`.
- Icon fonts: Generated using [Fontello](https://fontello.com/). Contains icons from:
  - MFG Labs: Licensed under the [SIL Open Font License](http://scripts.sil.org/OFL). See the full terms in `third_party/fontello/LICENSE.txt`.
