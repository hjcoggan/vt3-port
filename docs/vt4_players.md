# VT4 players in VT3 (shelved 2026-10-24)

Experiment: bring VT4-only players (Djokovic first) into VT3. Shelved by the owner as more work
than it is worth. This note records what was found so it can be picked up again. Game data stays in
`../dump`; the scripts are in `tools/formats/vt4/`.

## VT4 side (decoded)

Player models are SDO files (ColladaToSdo, big-endian) inside the VT4 archives, one per player:
Djokovic is `dump/vt4/entries/b7807.bin`. `dump/vt4players/players.csv` lists all 156 player files
(27 codes; VT4-only: DJV, MRY, DEL, BEC, EDB, WOZ, KUZ, IVA, RAF, CHA, COU, GON, KOH, ROB, ...).

`vt4mesh.py` reads:
- `VertexBuffer`: u8 flags, BE u32 count, BE u32 bytes, u8 parts, per part (BE u32 offset, BE u32
  bytes, u8 n, n x (u8 type, u8 comps)). Type 4 = float32, 2 = int16 (normal and tangent / 16384,
  uv / 65536 + 0.5), 1 = u8 (bone indices; weights summing to 255).
- `IndexBuffer`: BE u16 triangle strips, relative to the vertex-buffer part.
- `Batch`: the part's bone list (u16-length paths) and one 3x4 inverse bind matrix per bone.
Meshes per player: Face, Body, Wear, Pants, Shoes, Accessory, Eyelash, Hair_fix (+ `_lodL`), Shadow.
`export_obj.py ENTRY OUTDIR` writes them as OBJ; `obj_render.py` previews them.

## VT3 side (partly decoded)

- Ancic's geometry is `game.xpac` entry 746: 19 concatenated blocks
  `[u16 triangle list][zeros to a 16-byte offset][vertices]` (strides 44/48/40/28/24/20/8), then 28
  DDS at 4 KB slots from 0x8E000. `vt3_player.py` parses them. Bone byte = global slot * 3.
- The game does not scan the geometry: entry 247 is Ancic's model descriptor (mesh names, materials,
  index counts, block offsets, bounding spheres, cloth data, bones). Not decoded. Pointing one
  player's slot at another's geometry breaks rendering for that reason.
- Shirt and shorts are cloth-simulated (blocks 1, 2, 15-17 and descriptor data); not decoded.

## The skeletons match

VT3's player body (entry 746 block 0) equals VT4's Shadow mesh vertex for vertex: the same rig and
bind pose, about 18.2 units tall. VT3 bone slot -> VT4 bone: 0 Hip, 1-3 Spine/Spine1/Spine2,
4-24 right arm (Shoulder, Arm, ForeArm, Hand, fingers Thumb/Index/Middle/Ring/Pinky 1-3, ForeArmRoll,
ArmRoll), 25-45 the left arm (fingers ordered Thumb, Pinky, Ring, Middle, Index), 46 Neck, 47 Head,
48-52 left leg (UpLeg, Leg, Foot, ToeBase, LegRoll), 53-57 right leg. VT4's face bones fold into Head.

## Where it stopped

`head_swap.py GAME_XPAC` writes Djokovic's head (Face_lodL), hair (Hair_fix_lodL) and face, hair and
eye textures into entry 746 in place, padded to Ancic's exact block sizes so the descriptor stays
valid. It was never seen in a match: the Tournament route draws a random opponent and Ancic did not
come up before the project was shelved. Next steps would have been: see it in game, decode the
descriptor (to allow other sizes, the full-detail head and the body), then the cloth.
