# Lights (partly understood)

Lights are components of entities (class `CID_LightData`, 2,211 of them). Observed on sector A1:

| Offset | Content |
|---|---|
| 0x20 | pointer to the owning entity's component list |
| 0x40 | scale (x, y, z, 0) |
| 0x50 | rotation, quaternion (x, y, z, w) |
| 0x60 | position, in the owner's coordinates |
| 0x90 | two distances, probably where the light starts to fade and where it ends (50 and 800 for a street lamp spot) |
| 0xA0 | two cone angles in radians (inner, outer: 30 and 85 degrees for the same spot) |
| 0xB0 | the cosines of their halves |
| 0xC0 | a float (14.3, 50 or 0 in the samples), then a small integer (2) |
| 0xC8 on | (pointer, count) pairs to parameter blocks, not decoded |

The light volumes the deferred renderer draws are also in the sector geometry (effects `deferred_light_*`);
their "material" is a different structure from a surface material.

A light's colour is in its property set (`Light.COLOR`, as hue in degrees, saturation, value; see
`docs/formats/scripts.md`), for example 53, 0.4, 1.5 for a park lamp. The light volumes are shared shapes; they are
not tied to particular lights.

## Where a sector's lights are

Light objects sit inside prototypes (merged files, see `xpp-pack.md`), in the prototype's own space: a park
lamp's prototype has one point light 488 units above its base. The sector places each prototype with a record
of 0x4C bytes:

| Offset | Content |
|---|---|
| 0x00 | two zero words |
| 0x08 | rotation, nine floats (rows of a 3 x 3 matrix) |
| 0x2C | position (x, y, z) in the sector's coordinates |
| 0x38 | the word 0xFFF |
| 0x3C | a float (2, 0.5 or 32 in the samples; meaning not known) |
| 0x40 | name of the placed entity |
| 0x44 | name of its prototype |
| 0x48 | a word 0xFFxx |

Verified on one sector: 826 such records in runs, among them six park lamps standing upright on the ground;
light position = rotation x local position + record position puts each light at its lamp's head. The table that
owns the records (count, pointer) has not been found, so the reader finds them by their shape; it skips records
whose matrix is not a pure rotation (about a third of them, mostly small statue parts).

The engine adds these lights to the sector's vertices when a sector is built (`engine/world/sector_lights.*`):
full strength within the first distance, fading to nothing at the second, no shadows. Spot cones are not
applied yet.
