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
their "material" is a different structure from a surface material. Colours have not been located yet.
