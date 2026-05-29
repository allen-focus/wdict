# /// script
# dependencies = [
#   "pillow"
# ]
# ///

# ----------------------------------------------------------------------
# author: mmozeiko
# url: https://gist.github.com/mmozeiko/47cf52adc39441d512c28efb2efb3a20
# edited by: Gleko
# ----------------------------------------------------------------------

# packs multiple images (bmp/png/...) into ico file
# width and height of images must be <= 256
# pixel format of images must be 32-bit RGBA

import argparse
import struct
import os
from PIL import Image  # https://python-pillow.org/


def pack(input, output):
    count = len(input)

    with open(output, "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, count))
        offset = struct.calcsize("<HHH") + struct.calcsize("<BBBBHHII") * count

        for i in input:
            size = os.path.getsize(i)
            img = Image.open(i)
            w = 0 if img.width == 256 else img.width
            h = 0 if img.height == 256 else img.height
            if w > 256 or h > 256:
                exit("Image max size is 256x256")

            f.write(struct.pack("<BBBBHHII", w, h, 0, 0, 0, 32, size, offset))
            offset += size

        for i in input:
            f.write(open(i, "rb").read())


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="pack multiple images into ico file")
    ap.add_argument("input", type=str, nargs="+", help="input images")
    ap.add_argument("-o", "--output", help="output file")
    args = ap.parse_args()
    pack(args.input, args.output)
