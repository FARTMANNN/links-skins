# Link Skins

A skin picker for Link in the Ocarina of Time remake on PC, for the mod loader of the
[OoT Co-op mod](https://github.com/daurosk/oot-remake-multiplayer). Version 0.4.4. Early development: expect bugs.

It adds a "Link's skin" setting to the Mods tab. Every skin folder you put in `link-skins\skins` shows up as an entry.

This repository holds the mod's code only. **It has no game files and no skins**: a skin is made from your own copy of the game, and those files must not be uploaded here.

## Install

1. You need the OoT Co-op kit, version 0.11.3 or newer. Link Skins does not work on plain Longshot.
2. Build `link_skins.dll` (see Build) or take it from a release.
3. Copy the `mod` folder into the kit as `hookshot\game\mods\link-skins`.
4. Put your skins in `hookshot\game\mods\link-skins\skins`.
5. Start the game. After skins are added or removed it asks for one restart.

## Use

- Pause menu > Mods > Link Skins > Settings > Link's skin.
- Link's body changes when you save and quit to the title screen and load your save again, or when he changes age. No restart of the game.
- The Goron and Zora tunics and the gauntlets change when they are put on again.
- "The game's own Link" takes the skin off.

## Making a skin

A skin is a folder in `skins`. The folder's name is its id: letters, digits, `.`, `_` and `-`.

```
skins\my-skin\skin.json      optional: name, author, version, description, colour
skins\my-skin\romfs\...      the replaced files, laid out as the game's romfs
```

An existing model-replacer mod (a folder holding `romfs`) works as a skin as it is. See [docs/skins.md](docs/skins.md).

## Co-op

Until the co-op mod can tell players which skin each one wears, each skin stands for one tunic colour, named in the skin list. Other players who have the same skins installed see your skin when your tunic colour is that colour. The setting "Skins on other players by tunic colour" turns this off.

Known limit: the other players' Goron and Zora tunics and gauntlets show as your own skin's.

## Build

```
sh build.sh
```

It cross-compiles `mod/link_skins.dll` with zig (installed through pip) and zstd 1.5.6. `src/test_main.c` is a stand-in for the mod loader that runs the same code on Linux.

## Licence

GPL-3.0-or-later, as the co-op mod. `src/coop_mod_api.h` is from the co-op mod. zstd is BSD licensed.
