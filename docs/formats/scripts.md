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

What has been read so far (from the interpreter and checked against the opening mission's data):

- It is a typed stack machine with nested scopes. Instructions reserve and release stack bytes, push
  constants (inline, any size), load and store scope variables by (scope depth, offset), get and set
  engine properties, branch, call script functions and call engine functions.
- A script function is a 16-byte closure value: a code pointer plus the scope it was created in. Functions
  start with an entry instruction carrying their frame size and end with a return.
- **Script objects.** A script object is an initialisation block that stores constants and closures into
  its scope variables, followed by a symbol table of 16-byte entries: name, offset, type, zero. The table
  names every member variable and method, so every function can be named. The opening mission has 319
  functions (`OnMissionStart`, `Setup`, `PlayDialog`, `RunBattle`, per-sector `SetupHex...`).
- **Links to the engine.** `SPLP` has three sections, counted by its first three words: property links of
  3 words (offset, class, property), property links of 7 words, and function links of 6 words (the offset
  of a call instruction, a slot, the function's name) with extra words after some. The opening mission
  links 716 calls to 81 engine functions; the most used are `Sleep`, `CtsThreadCreate` (start a script
  thread), `RetractThread`, `ObjFind`, `HeroFind`, `MissionTriggered`, `SndEffect`, `EventCreate`,
  `WaitForEvent`. Scripts are cooperative threads that sleep and wait.
- **Clip events.** Clip objects can carry an event track (a fraction of the clip's length and a name);
  the opening shot's clip names six moments of the blast. Script objects receive them as labels.

**The opening sequence**, as its script function reads: a screen fill; time-of-day adjustment; after a
few seconds the logo-and-prompt object is triggered; the script polls the pad each frame until the start
button is pressed; the prompt is removed, the explosion's music list and a sound mix start, and the first
camera plays its `start` clip (the blast, with its six labelled moments); the district's power grid is
switched off; a second camera shot follows with another sound mix; fills cover the cut; the mission's
first subtask is completed and a thread warps the player on. Not yet resolved: the script functions called
between those steps, and what each label does.

**Missions.** Each mission has its own `.sprig.xpp` pack (140 of them). They hold the mission's objects:
transform markers (`XoData`), trigger and camera volumes, logic lists (`LoListData`), user-interface
objects and the mission's sound banks, and one `MissionData` object. `EMPIRE.xpp` holds the city-wide
task graph (`TaskGraphData`) and mission start points.

Not decoded: most of the instruction set, how scripts attach to objects and events, the task graph, and
how missions are sequenced.
