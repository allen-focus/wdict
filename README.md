# Build

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
