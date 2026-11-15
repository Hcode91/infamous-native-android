# Animation (partly understood)

Clip objects (class `CID_AnimationClipData`) form blend trees: most are small nodes (0x60 bytes) that
point at other clips with weights and rates. The key frames themselves are in blocks that start with the
magic `CD` (`0x4344`), version `0x0107`, found inside clip objects (170 of them in the cop variants
pack). The game's own strings name the library they come from (`ice/anim`).

Block header (big-endian), verified on several blocks against their spacing and frame counts:

| Offset | Content |
|---|---|
| 0x00 | `CD`, u16 version 0x0107 |
| 0x04 | u16 joints (117, the animated joints of the skeleton), u16 float channels (29) |
| 0x08 | u32 block size in bytes |
| 0x0C | u16 (2), u16 frame count |
| 0x1C | f32 frames per second (30.0) |
| 0x20 | u32 frame count, f32 1 / (frames - 1), f32 frames - 1, f32 1 / fps |
| 0x30 | table of section offsets and sizes |
| 0x50 | bit mask of the joints the clip drives (117 bits), as in the game's `Valid Joints` debug output |

After the mask come lists of 16-bit commands (values like 0x0316, 0x041B, 0x060B followed by counts and
offsets) and lists of output offsets in steps of 0x30, the size of one joint transform. The commands
select how each group of channels is stored (constant, key-framed, compressed); the compressed data
follows. The command set is not decoded yet.

Clip blocks match the joint hierarchy described in [characters.md](characters.md): 117 animated
joints and 29 more.
