# PSARC archives

The game's world data is in two archives, `infamous1.psarc_s` and `infamous2.psarc_s`.

**Verified** (both archives read completely with `engine/assets/psarc.cpp`):

- Standard PSARC, version 1.3, `zlib` compression, 65,536-byte blocks, archive flags 3.
- All integers are big-endian.
- Header (32 bytes): `PSAR`, version major/minor (u16 each), compression tag (4 chars), TOC length (u32,
  counted from the start of the file), TOC entry size (u32, 30), entry count (u32), block size (u32),
  flags (u32).
- TOC entry (30 bytes): MD5 of the name (16), index of the first block (u32), uncompressed size (u40),
  file offset (u40).
- After the entries: one stored size per block, 2 bytes each for this block size. A stored size of 0 means
  a full block stored without compression.
- Entry 0 is the manifest: the paths of entries 1..n, separated by `\n`, each starting with `/`.
- The archives hold 492 and 1,804 files. Every one is a `.xpp` pack (see `xpp-pack.md`).
