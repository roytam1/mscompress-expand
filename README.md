# MS COMPRESS / EXPAND reconstruction (C89)

One compressor and one decompressor, written in C89 and built with MSVC6,
covering all the SZDD and KWAJ formats. Validated against the original
DOS executables with the supplied `msdos.exe` (MS-DOS Player).

## Programs

| Program | Source | Formats |
|---------|--------|---------|
| `COMPRESS.EXE` | `compress.c` | writes SZDD A (`-a1`), SZDD B (`-a5`), KWAJ 2 (`-a2`), KWAJ 3 (`-a3`, default), KWAJ 4 (`-a4`) |
| `EXPAND.EXE` | `expand.c` | reads SZDD A/B, SZDD-QBasic and KWAJ methods 0-4 |

Shared code:

| File | Purpose |
|------|---------|
| `mscompress.c` / `.h` | decoders, C89 port of the original C++ reference |
| `msinflate.c` / `.h` | small raw-DEFLATE reader (no zlib) for KWAJ 4 |
| `lzencode.c` / `.h` | hash-chain LZ token parser used by the compressor |
| `BUILD.BAT` | MSVC6 build script |

`MsCompress.h` and the IDA listings are kept for reference. The build
writes to `build\`, leaving the original EXEs in the project root
untouched.

## Algorithms

* **SZDD** — LZSS, 4096-byte window prefilled with spaces. Mode `A` uses
  write pointer `4096-16`; mode `B` (header byte 8 = `'B'`, as in
  `APPS.IN_`) uses `4096-18`, like KWAJ method 2.
* **KWAJ 2** — same LZSS, write pointer `4096-18` (the "Steven Zeck" method).
* **KWAJ 3** — LZSS plus five canonical Huffman trees. The writer builds
  frequency-derived (Shannon-Fano) trees and emits them with type-2
  run-length encoding, instead of the earlier uniform trees (the "Jeff
  Johnson" method, COMPRESS default).
* **KWAJ 4** — MS-ZIP: 32 KiB raw-DEFLATE blocks sharing one history window,
  written with dynamic Huffman trees and code-length RLE (symbols 16-18).
  Compatible with `DECOMP.EXE` and `kwaj4.py`.
* KWAJ 0/1 (stored / XOR) are decoded but not written.

The streams are format-correct rather than byte-identical to the
originals, but every one is accepted by the original DOS programs. On
`LICENCE.txt` (18,340 bytes) the current sizes are:

| Method | This project | Reference |
|--------|--------------|-----------|
| SZDD A (`-a1`) | 8,586 B | 8,617 B (COMPRE10) |
| KWAJ 2 (`-a2`) | 8,590 B | 8,597 B (sample) |
| KWAJ 3 (`-a3`) | 7,588 B | 7,601 B (COMPRESS) |
| KWAJ 4 (`-a4`) | 7,201 B | 6,955 B (`kwaj4.py`, zlib -9) |

## Build and use

```
BUILD.BAT
```

calls `VCVARS32.BAT` and produces `build\COMPRESS.EXE` and
`build\EXPAND.EXE`. All sources are strict C89 and build with MSVC6 `/W3`
without errors. These are 32-bit console programs; the originals were
16-bit DOS executables, but the command lines, messages and file formats
are compatible.

```
EXPAND   [-r] Source Destination
COMPRESS [-aAlg -bceflq$ ...] srcArg [destArg]
```

`-r` derives the expanded name from the compressed one (restoring the
character replaced by `_`/`$` and completing two-letter extensions).

## Verification

* All four supplied samples (`LI-szdd.tx_`, `LI-kwaj2/3/4.tx_`) expand to
  exactly `LICENCE.txt` (18,340 bytes).
* The SZDD type B sample `APPS.IN_` expands to 14,729 bytes, matching the
  size in its header; `-a5` writes the same variant and round-trips.
* Files written with `-a1`, `-a2` and `-a3` are decompressed correctly by
  the original `EXPAND.EXE`; `-a4` files by the original `DECOMP.EXE`
  (verified after the Huffman improvements).
* Files produced by the original `COMPRESS.EXE` and `COMPRE10.EXE` are
  decompressed correctly by `build\EXPAND.EXE`.
