# Scripts and object properties (partly understood)

**Property sets.** Objects carry lists of property assignments that the game binds at load time. Each
entry is 20 bytes: a header word `0x23 00 tttt` (`tttt` is the value type: 0x0001 boolean, 0x0002
integer, 0x0003 float, 0x1012 enumeration or flags), three words the game fills in, and the value. Lists
end with `0xB1000000`. The `SPLP` fixup chunk names, for each entry, the class and the property it sets:
in sector A1 a camera volume sets `MODES` = 4, `FIELD_OF_VIEW` = 0.9, `SWITCH_IN_AIR` = 0 and
`PRIORITY` = 0. See [xpp-pack.md](xpp-pack.md).

**The script machine (located, being decoded).** Property sets are programs for a bytecode machine, and
so is all mission logic. Instructions are 32-bit words with the opcode in the top byte; some are followed
by operand words (a constant, a field offset, or the three words that `SPLP` links to a class and a
property). The game's interpreter has been found in the executable: it reads the opcode byte at the
program counter and dispatches through a table of 185 entries (0x00..0xB8, 133 distinct handlers, four of
them invalid). Seen so far, provisional until every handler has been read: constant loads (0x01 object
id, 0x02 boolean, 0x03..0x0A typed), field loads (0x1C), property set and get by type (0x23..0x2D), method
calls by name (0x37, 0x3E), branches (0x43..0x45), native calls (0x88) and the list end (0xB1).

The opening mission's pack (`msn_jb_intro.sprig.xpp`) carries 5,143 property links. Its script names the
mission's checkpoints, its cameras, the explosion's music list, the first controller prompts and the
player's ability switches, so the opening sequence is this script: it cannot be reproduced faithfully
without running it.

**Missions.** Each mission has its own `.sprig.xpp` pack (140 of them). They hold the mission's objects:
transform markers (`XoData`), trigger and camera volumes, logic lists (`LoListData`), user-interface
objects and the mission's sound banks, and one `MissionData` object. `EMPIRE.xpp` holds the city-wide
task graph (`TaskGraphData`) and mission start points.

Not decoded: most of the instruction set, how scripts attach to objects and events, the task graph, and
how missions are sequenced.
