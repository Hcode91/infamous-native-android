# Collision

Verified on sectors A1 and B12 of the EU disc release.

A static object (class `CID_SoData`) can carry collision polygons. Each city sector has one static object
with collision and no drawn pieces (named `__merged_statics_phys`): the sector's collision, already in
the sector's coordinates (so it is moved by the sector's hex cell like the geometry). Other static
objects with collision are props (cranes, generators, street lights, facades), whose collision is in
their own coordinates and is placed with them.

Object +0x1C0 points at sub-records of 0x70 bytes; +0x1C4 is their count (94 in A1, 99 in B12). A
sub-record:

| Offset | Content |
|---|---|
| 0x00 | bounding sphere: centre x, y, z, radius |
| 0x10 | pointer, count: vertices, three big-endian floats each |
| 0x18 | pointer, count: loop entries, `u16 vertex, u16 neighbouring polygon` (0xFFFF: open edge) |
| 0x20 | pointer, count: polygons, `u16 first loop entry, u16 (unknown, increasing), u16 flags` |
| 0x28 | pointer, count: materials (not decoded) |
| 0x30 on | further tables (not decoded) |

A polygon's loop runs from its first entry to the next polygon's first entry and repeats its first vertex
at the end; polygons are convex and are split into triangle fans. In A1 the sector collision is 6,882
vertices and 10,867 triangles; drawn, it shows the piers, pavements, steps and simplified buildings of
the sector.

**Use.** The engine reads it with `readCollision` and answers "highest surface under a point" queries with
`GroundGrid`, which the world viewer uses to keep the camera target on the ground.

**Not known:** the polygon flags and materials (surface types), the tables from +0x30 of a sub-record
(probably a search tree), and how props' collision is placed.
