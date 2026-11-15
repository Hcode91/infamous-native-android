# Scripts and object properties (partly understood)

**Property sets.** Objects carry lists of property assignments that the game binds at load time. Each
entry is 20 bytes: a header word `0x23 00 tttt` (`tttt` is the value type: 0x0001 boolean, 0x0002
integer, 0x0003 float, 0x1012 enumeration or flags), three words the game fills in, and the value. Lists
end with `0xB1000000`. The `SPLP` fixup chunk names, for each entry, the class and the property it sets:
in sector A1 a camera volume sets `MODES` = 4, `FIELD_OF_VIEW` = 0.9, `SWITCH_IN_AIR` = 0 and
`PRIORITY` = 0. See [xpp-pack.md](xpp-pack.md).

**Instruction streams.** Several classes (particle and effect emitters, camera volumes, some scripted
objects) also hold streams of 32-bit words that look like register-machine instructions: an opcode in
the top byte (0x3B, 0x1C, 0x22, 0x3A, 0x88, 0x8D and others recur) and register or immediate fields
below. Their meaning has not been worked out; the interpreter is in the game executable.

**Missions.** Each mission has its own `.sprig.xpp` pack (140 of them). They hold the mission's objects:
transform markers (`XoData`), trigger and camera volumes, logic lists (`LoListData`), user-interface
objects and the mission's sound banks, and one `MissionData` object. `EMPIRE.xpp` holds the city-wide
task graph (`TaskGraphData`) and mission start points.

Not decoded: the instruction set, the task graph, and how missions are sequenced.
