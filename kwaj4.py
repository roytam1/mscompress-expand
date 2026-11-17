#!/usr/bin/env python3
"""
Experimental KWAJ method-4 (MSZIP/DEFLATE) encoder.

Usage:
    python kwaj4.py inputfile [outputfile]

The output uses 32 KiB MSZIP blocks: <uint16 block-size><"CK"><raw DEFLATE>.
Each block's size includes the two-byte "CK" marker. A zero-size block ends
the stream.

IMPORTANT: The supplied Office 4.2 sample's DWORD at header offset 4 is
0xD127F088. Its meaning is not established here, so this encoder preserves
that observed value. Validate output with decomp14.exe and libmspack before
relying on it for production use.
"""
import argparse
import os
import struct
import sys
import zlib

BLOCK_SIZE = 32768
# Header layout observed in the supplied Office 4.2 sample:
# "KWAJ", unknown DWORD, method=4, header-size=14, flags=0.
SAMPLE_UNKNOWN_DWORD = 0xD127F088


def make_blocks(data):
    """Return MSZIP blocks with a rolling 32 KiB DEFLATE dictionary."""
    result = bytearray()
    dictionary = b""
    for pos in range(0, len(data), BLOCK_SIZE):
        chunk = data[pos:pos + BLOCK_SIZE]
        compressor = zlib.compressobj(
            level=9,
            method=zlib.DEFLATED,
            wbits=-15,
            memLevel=8,
            strategy=zlib.Z_DEFAULT_STRATEGY,
            zdict=dictionary if dictionary else b"",
        )
        compressed = compressor.compress(chunk) + compressor.flush()
        # MSZIP block size counts the CK signature plus compressed bytes.
        payload = b"CK" + compressed
        if len(payload) > 0xFFFF:
            raise ValueError("Compressed MSZIP block exceeds 65535 bytes")
        result += struct.pack("<H", len(payload))
        result += payload
        dictionary = (dictionary + chunk)[-32768:]

    # KWAJ/MSZIP stream terminator observed in the sample.
    result += b"\x00\x00"
    return bytes(result)


def encode(data):
    header = (
        b"KWAJ"
        + struct.pack("<I", SAMPLE_UNKNOWN_DWORD)
        + struct.pack("<HHH", 4, 14, 0)
    )
    return header + make_blocks(data)


def main():
    parser = argparse.ArgumentParser(
        description="Compress a file into an experimental KWAJ method-4 file."
    )
    parser.add_argument("input", help="input file")
    parser.add_argument("output", nargs="?", help="output file (default: input + '.kwj')")
    args = parser.parse_args()

    output = args.output or (args.input + ".kwj")
    try:
        with open(args.input, "rb") as f:
            data = f.read()
        encoded = encode(data)
        with open(output, "wb") as f:
            f.write(encoded)
    except (OSError, ValueError, zlib.error) as exc:
        print("kwaj4: error: {}".format(exc), file=sys.stderr)
        return 1

    print("{} -> {} ({} -> {} bytes)".format(
        args.input, output, len(data), len(encoded)
    ))
    return 0


if __name__ == "__main__":
    sys.exit(main())
