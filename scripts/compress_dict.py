# /// script
# dependencies = [
#   "pyzstd"
# ]
# ///

"""compress_dict.py — Compress a file using zstd compression.

Usage:  python scripts/compress_dict.py <input> <output>
Example:
  python scripts/compress_dict.py data/dict.bin data/dict.bin.lz4
"""

import pyzstd
import sys
from pathlib import Path


def compress(input_path: str, output_path: str):
    data = Path(input_path).read_bytes()
    compressed = pyzstd.compress(data, 19)

    out = Path(output_path)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(compressed)

    orig_kb = len(data) / 1024
    comp_kb = len(compressed) / 1024
    ratio = len(compressed) / len(data) * 100
    print(f"Input:     {orig_kb:.1f} KB")
    print(f"Output:    {comp_kb:.1f} KB ({ratio:.1f}%)")
    print(f"Saved:     {orig_kb - comp_kb:.1f} KB")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"Usage:  python {sys.argv[0]} <input> <output>")
        sys.exit(1)
    compress(sys.argv[1], sys.argv[2])
