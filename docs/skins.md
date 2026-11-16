# Skins

## Layout

```
mods\link-skins\
  mod.json            rewritten by the mod when the skin list changes
  link_skins.dll
  skins\<id>\skin.json
  skins\<id>\romfs\...
  romfs\              every skin's files, kept by the mod (do not edit)
  romfs.txt           what romfs\ holds
```

- A folder whose name starts with `_` or `.` is left out.
- A skin needs at least one file in its `romfs`.
- Up to 63 skins.

## skin.json

```json
{
  "format": 1,
  "name": "My skin",
  "author": "me",
  "version": "1.0",
  "description": "One line.",
  "colour": "purple",
  "sizes": { "Model/Player_Adult.Player_Adult.bfres": 9148520 }
}
```

- `colour`: the co-op tunic colour that stands for the skin (black, blue, cyan, orange, pink, purple, red, white, yellow). Left out, one is given.
- `sizes`: size-table entries set by hand, by romfs path without `.zs`. Only needed when the mod's guess is too small.

## How files are placed

Every skin's files are in the mod's `romfs` all the time, so the game lists them at its start.

- A file only one skin has and the game has not (its own textures) is always that skin's.
- A file the game has too (Link's model, the tunic models) holds the chosen skin's copy, or the game's own.

Picking a skin swaps the contents of those files. Adding or removing a skin changes the list, which needs one restart.

## Resource size table

A skin does not ship the game's resource size table. The mod builds one that covers every installed skin: the game's table, the larger entries of every other mod's table, and an entry per skin file.

- A texture's entry is its unpacked size + 280.
- A model no larger than the game's keeps the game's entry.
- A larger or new file gets a guess with room to spare; `sizes` in skin.json overrules it.
- A table found inside a skin's `romfs` is only read for its entries.

## Tips for skin makers

- Give new or changed textures new names instead of replacing the game's. A replaced game texture is shared by everything else that uses it.
- Keep Link's skeleton. Animations and the co-op pose sync depend on it.
