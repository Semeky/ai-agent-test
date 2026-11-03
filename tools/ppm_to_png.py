#!/usr/bin/env python3
"""Convert the demo's binary PPM (P6) frame to PNG using only the Python stdlib.

The renderer intentionally has no external dependencies, but GitHub and most
image viewers prefer PNG.  This helper is used by `make screenshots`.

Usage: python3 tools/ppm_to_png.py input.ppm output.png
"""

import struct
import sys
import zlib


def read_ppm(path: str) -> tuple[int, int, bytes]:
    data = open(path, "rb").read()
    if not data.startswith(b"P6"):
        raise ValueError("not a binary PPM (P6) file")

    tokens: list[bytes] = []
    index = 2
    while len(tokens) < 3:
        while index < len(data) and data[index : index + 1].isspace():
            index += 1
        if index < len(data) and data[index] == ord("#"):
            while index < len(data) and data[index] not in b"\r\n":
                index += 1
            continue
        start = index
        while index < len(data) and not data[index : index + 1].isspace():
            index += 1
        if start == index:
            raise ValueError("truncated PPM header")
        tokens.append(data[start:index])

    # Exactly one whitespace byte separates the header from the pixel data.
    index += 1
    width, height, max_value = (int(token) for token in tokens)
    if max_value != 255:
        raise ValueError("only 8-bit PPM files are supported")
    pixels = data[index:]
    expected = width * height * 3
    if len(pixels) != expected:
        raise ValueError(f"expected {expected} pixel bytes, got {len(pixels)}")
    return width, height, pixels


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    checksum = zlib.crc32(kind + payload) & 0xFFFFFFFF
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", checksum)


def write_png(path: str, width: int, height: int, pixels: bytes) -> None:
    stride = width * 3
    raw = bytearray()
    for row in range(height):
        raw.append(0)  # filter type: none
        raw += pixels[row * stride : (row + 1) * stride]

    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    png = (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", header)
        + png_chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + png_chunk(b"IEND", b"")
    )
    with open(path, "wb") as output:
        output.write(png)


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} input.ppm output.png", file=sys.stderr)
        return 2
    width, height, pixels = read_ppm(sys.argv[1])
    write_png(sys.argv[2], width, height, pixels)
    print(f"wrote {sys.argv[2]} ({width}x{height})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
