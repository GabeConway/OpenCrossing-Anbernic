# Game-side knowledge (decomp / emu64 / data)

- Game code: `src/` (decomp, gnu89 C, relies on UB — see the CMake UB-guard
  flags; treat as vendored, edit only for real bugs, keep upstream-diffable).
- **emu64** (`src/static/libforest/emu64/`): the game's own N64→GC graphics
  emulation layer — AC is an N64 game running on a GC shim. It emits GX
  calls; our pc_gx layer sits below it. It omits GXEnd (why
  `pc_gx_flush_if_begin_complete` exists) and reuses memory buffers for
  different textures/TLUTs (why the texture cache hashes contents).
  Known past fix: `mw_data` → `moveword->data` (emu64.c:5537, armhf build).
- **32-bit assumption everywhere**: JSystem/decomp casts pointers to u32.
  Everything must build ILP32 (armhf ok; arm64 branch upstream used other
  tricks). Never store pointers in u32 in new code paths without checking.
- Supported image: GAFE01 USA Rev 0 only. Assets read directly from the iso
  at runtime (pc_disc.c/pc_dvd.c, synchronous fread — SD card reads are a
  possible minor stutter source on first area loads).
- Saves: GCI format in `save/`, Dolphin-compatible both directions.
- Texture packs: Dolphin format (XXHash64 names, DDS) in `texture_pack/`;
  `preload_textures` setting: 0 off, 1 preload at boot, 2 preload+cache file.
  Perf cost on device — off by default.
- Settings quirks: `fps_target=6` = dynamic (enum in pc_settings.h);
  `window_size` is a preset index (5=custom); overlay menu (Select button)
  tabs: VIDEO/AUDIO/CTRL/DEBUG/PERF — menu item↔tab mapping in
  pc_overlay.c `menu_item_tab[]`.
- Model viewer debug mode: `--model-viewer [index]`.
- Built-in NES emulator: NOT implemented on PC — `ac_my_room.c:2111`
  `#ifdef TARGET_PC` stubs all famicom furniture to the "no software"
  message (kb/issues.md 2026-07-19). `pc_gx_restore_after_nes()`
  (pc_gx.c:522) is uncalled scaffolding; if an NES core is ever wired in
  with its own GL objects, call it after the emu runs — it rebinds ours
  and dirties all state, and any new global GL state must be handled
  there too.
- **Campsite villagers cannot move in — by design** (verified in source
  2026-07-19 after a player report of talking/playing games for an hour
  with no move-in offer): the camper is special NPC type 0xD05E
  (m_npc.h:24 — 0xD000 event NPCs, vs 0xE0xx regular villagers); move-in
  runs through the grow system (`mNpc_SetGrowNpc` m_npc.c:4382 sets
  `moved_in` only for regular types; grow_list.c eligibility table covers
  the 236 regular NPCs only) and the contract talk trigger
  (ac_npc_talk.c_inc:446) never fires for special types. No pc/ override
  touches any of this. Same as the original GC game — campers
  (tent/igloo) are minigame/item visitors only; camper move-in is a
  New Leaf-era mechanic. Not village-rating, not RNG bad luck. Save
  editors are the only route, and see kb/issues.md — unvalidated edited
  saves currently crash the port.
- `--time HOUR` overrides in-game hour; handy for testing time-of-day
  rendering (fog/lighting TEV configs differ by hour → different shaders).
