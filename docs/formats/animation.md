# Animation clips (decoded, with two open points)

Status: a clip decodes to joint transforms and a character plays it. **Verified** on the 170 clips of the
cop variants pack: every clip parses, every key region has exactly the size the layout predicts, and decoded
poses are right by construction tests (below). **Not known:** two flag bits on some rotation components,
how the game itself solves the legs, and how blend nodes mix their clips. Float channels are parsed past
but not used.

## Named animations and clip blocks (decoded)

Animation nodes are objects whose names start `anim_`: plain ones (`AnimationData`, class 0x1EC) and
blend nodes (`AnimBlendStrafeData` 0x1EF, `AnimBlendRunData`, `AnimClipAimData`, parkour, melee...). A node
holds (pointer, count) pairs to lists; a list entry points at a slot of the pack's object table, and the
slot at a clip object (`AnimationClipData`, class 0x229, 0x70 bytes). A clip object's own data starts at
the pointer at +0x34 (16-byte joint masks) and holds records `pointer to a clip block, f32 duration, ...`;
blend sets hold several (one per direction or speed).

- The shared pack has 880 nodes, 2,701 clip objects and 3,447 clip blocks; every block is reached.
- The player's nodes are named `anim_messenger_...` (257 of them): `action_idle` (clip objects `settle`,
  `crunch`, `standing`, `standing_dust_off`...), `strafe_run`, `blended_walk`, `jump_right_foot`...
- A block's root transform (header +0x18) gives its movement over the clip. The player's run set has
  blocks moving (330, 0, 0), (-300, 0, 0), (0, 250, 0), (0, -250, 0) in 0.67 s and a sprint of (400, 0, 0)
  in 0.5 s: +X is forward, and the plain run is 495 cm/s.
- How blend nodes weight their clips (direction, speed, aim angles) is **not decoded**; this engine picks
  single blocks (the idle called `standing`, the forward run).

The joint hierarchy a clip belongs to is the one whose animated joint count (`JH` +0x04) equals the
clip's joint count: the shared pack has 102 hierarchies; the player's has 246 joints, 125 animated.

The key frames are in blocks with the magic `CD` (`0x4344`), version `0x0107`, inside the clip objects'
data. The game's strings name the library (`ice/anim`).

## Block header (big-endian)

| Offset | Content |
|---|---|
| 0x00 | `CD`, u16 version 0x0107 |
| 0x04 | u16 joints the clip can drive (117: the animated joints of the skeleton), u16 float channels (29) |
| 0x08 | u32 block size |
| 0x0C | u16 (2), u16 frame count |
| 0x14 | u32 0x30; u32 offset of a 0x30-byte root transform (scale, rotation, translation), or 0 |
| 0x1C | f32 frames per second (30.0) |
| 0x20 | u32 frame count, f32 1 / (frames - 1), f32 frames - 1, f32 1 / fps |
| 0x30 | joint group: u32 offset, u16 size, u8 joints, u8, u32 offset of its key data, u16 key bytes per frame, u16 |
| 0x40 | float group: u32 offset, u16 size, u16 channels, u32 offset of its key data, u16 key bytes per frame, u16 |

Each group starts with a 16-byte mask of the outputs it drives (most significant bit first; the game's
debug text calls it `Valid Joints`), followed by a command list.

## Command lists

A list is a run of 16-bit words ending in a zero word. Each command is `argument count << 8 | function`
followed by its arguments. Arguments that locate data are offsets from the start of the command list
(group offset + 0x10) with the low four bits naming a buffer: 0 for the clip data, `0xC` for the output
pose. In the SPU program that runs these lists the function number indexes a table of handlers.

Outputs are addressed as `joint * 0x30 + part`, the part being 0x00 (scale), 0x10 (rotation quaternion) or
0x20 (translation): the layout of one joint transform. Lists of outputs are padded with `0x15F0`, the first
address past the 117 joints.

| Command | Arguments | Meaning |
|---|---|---|
| `0x0316` | n, out, data | n output addresses, then n constant vectors of three half floats |
| `0x0318` | n, out, data | n output addresses, then n constant rotations of 48 bits |
| `0x060B` | n (top bit set when there are rotations), out, widths, 0, 0, bytes per frame | unpack the key-framed channels; one byte per channel giving its bits per frame |
| `0x041B` | n, out, rows, 0 | scale and offset: 16 bytes per channel, then 4 bytes per channel of bit widths |
| `0x041E` | rotations, out, rows, start | 16 bytes per rotation channel; `start / 0x20` is the first rotation channel |
| `0x040F` | vectors, out, list | where the vector channels go |
| `0x0410` | rotations, out, list, start | where the rotation channels go |
| `0x0319` | n, out, data | float group: constant floats |
| `0x060C`, `0x0411` | | float group: key-framed floats and where they go (not decoded) |

Vector channels come first and are padded to an even count; rotation channels follow.

**48-bit rotation.** `[x:16][y:15][z:15][index:2]`, each stored component mapped to +-1/sqrt 2. The largest
component is left out and rebuilt from the other three. It belongs in slot `index` (0 = x ... 3 = w), and
the component that slot would have held is stored in the w position: decode `(a, b, c, rebuilt)` and swap
slots `index` and 3. Identity is `8000 8001 0003`. Verified against rotations of the same joints in other
clips, and by the cop's hands meeting on his pistol grip.

## Key-framed channels

Every frame takes the same number of bytes, so frame f starts at `key data + f * bytes per frame`. Inside a
frame the channels follow each other bit by bit, most significant bit first, each with three components.

- **Widths.** Four bytes per channel: `x bits, y bits, x + y, z bits`. The low six bits are the width. The
  top two bits of a width are flags found only on rotation components (about one component in six);
  **their meaning is not known** and the values look ordinary.
- **Scale and offset.** Two 64-bit words per channel, each holding three packed floats with an 8-bit
  exponent (bias 127). Scale: `[exp:8 mantissa:14][exp:8 mantissa:13][exp:8 mantissa:13]`, no sign.
  Offset: `[sign exp:8 mantissa:13][sign exp:8 mantissa:12][sign exp:8 mantissa:12]`.
  `value = offset + scale * stored integer`. Unused components have scale 1.0 and width 0.
- **Vectors** are used as they come (translations in centimetres).
- **Rotations.** Each rotation channel has 16 more bytes: two unit quaternions as signed 16-bit fractions,
  with their components interleaved (`ax bx ay by az bz aw bw`). The three values are a rotation vector v
  whose length is half the angle, and the rotation is `a * b * exp(v)` (Hamilton product, the same
  convention as the joint hierarchy).

How the rotation formula was found, since there is no reference decoder to compare with:
`a * b` matches the constant rotations the same joints have in other clips far better than any other
combination (median difference 0.001); for hinge joints (elbows, finger joints), whose stored values move
along one component only, the hinge axis comes out the same in every clip only with `exp(v)` on the right;
the largest |v| in the pack is 1.28, which rules out v being the vector part of a quaternion and fits half
an angle (limit pi/2). With all of it applied, both of the cop's hands close around the same point in the
aiming clips.

Between two frames this engine blends linearly (rotations along the shorter arc, then normalised). What the
game does between frames is not known.

## What clips drive, and the legs

A clip drives the first 117 joints of the 211-joint hierarchy. The other 94 are fixed offsets
(`adjust_...`) and helper joints (muscles, elbow and knee `fixers`, twists); they keep their rest
transforms here. The helpers are probably driven by a table in the hierarchy block that is not decoded.

Clips animate four target joints: `left_foot_foster`, `right_foot_foster`, `left_hand_foster`,
`right_hand_foster`, each with a helper child (`left_leg_ik`...). The hip, knee and ankle rotations stored
in clips do **not** put the feet on the foot targets, and in looping clips they are the only channels whose
first and last frames differ, so the game must place the legs from the targets. **Hypothesis, as
implemented:** the ankle is carried rigidly by the foot target (relative to the rest pose), the knee
direction is carried the same way by the helper joint, and hip and knee come from a two-bone solve. This
gives a believable run cycle and planted feet; it is a reconstruction, not the game's solver. Arms follow
their stored rotations (the wrists do not track the hand targets, which seem to be for held objects).

Not handled: the root transform at 0x18 (movement of the whole character over the clip), partial clips
layered over others, additive clips, blend nodes, float channels.

## Camera rigs (decoded)

Scripted shots are small joint hierarchies animated by ordinary clips. The opening mission's first shot
(`anim_intro`, clip objects `init`, `start` of 15 s and `pull_back`) uses four joints: 0 root, 1 the
camera (it looks along its +X with +Z up), 2 a child of the camera 150 m along +X (focus), 3 the field of
view as a rotation about Z (51 degrees at rest; the clip moves it between 24 and 61). The node points at
its hierarchy at +0x80. Positions are relative to the object named `camera_parent` in the same file of
the pack (at -21000, -18000, 0), which puts the shot at street level beside the mission's first
checkpoint and its `GUIDE_CAMERA_START` / `GUIDE_CAMERA_END` markers: the clip starts 2.4 m from the first
marker's height and ends within 2 m of the second. Whether the angle is the horizontal or the vertical
field of view is **not known**; this engine takes it as horizontal for a 16:9 picture.

## Tools

- `ina_tool clips <archive> <pack> [oid_manifest.txt] [clip] [frame] [out.obj]` lists a pack's clips, or
  poses the pack's first variant at a frame and writes it as an OBJ.
- `ina_tool anims <archive> <pack> <oid_manifest.txt> [name filter]` lists named animations with their
  clip objects, blocks, durations and root movement.
- `ina_tool pose <archive> <pack> <oid_manifest.txt> <variant> <animation> <block index> <frame> <out.obj>`
  poses one variant in one frame of a named animation.
