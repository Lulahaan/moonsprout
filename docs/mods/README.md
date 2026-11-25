# Making mods for Moonsprout

A Moonsprout mod changes the game's data files. It can change an arena, a cave, the tuning of
enemies and Pikmin, a texture or a list of treasures. It does this by giving the game another copy
of a file. The game reads the mod's copy instead of the one on your disc.

A mod never contains the game itself. You make the changed files from your own disc and ship only
those files. Do not share files you did not make or are not allowed to share.

This guide is for mod authors. The technical description is in
[../online/M5_2_MODS.md](../online/M5_2_MODS.md).

## 1. What a mod can change

- **Loose files.** Any file on the disc can be replaced, for example
  `user/Mukki/mapunits/caveinfo/*.txt` (caves), `user/Abe/vs/stages.txt` (the 2P Battle stage
  list) and `user/Abe/piki/*Parms.txt` (tuning).
- **New files.** A mod can add files that are not on the disc. Put new mode data under
  `user/mppm/`. The game finds a new file only if something asks for it by name, for example a
  stage list that names a new cave file.
- **Files inside archives.** A file inside a `.szs` or `.arc` archive can be replaced, for
  example `enemy/parm/enemyParms.szs` → `kochappy/enemyparm.txt`. See section 5.
- **Whole archives.** A whole `.szs` can also be replaced as a loose file.

A mod cannot change the game's code. Code mods are planned for later.

## 2. Folder layout

Mods live in the `mods` folder inside the Moonsprout data folder:

| System | Mods folder |
|---|---|
| Linux | `~/.local/share/mppm/mods/` |
| Windows | `%APPDATA%\mppm\mods\` |
| macOS | `~/Library/Application Support/mppm/mods/` |

The Mods window in the start menu has an **Open Mods Folder** button.

```
mods/
  load_order.txt                          which mods are on, and their order
  my-arena/                               one folder per mod, named after its id
    mod.json                              the manifest
    files/                                loose files; mirrors the disc's files/ folder
      user/Abe/vs/stages.txt
      user/Mukki/mapunits/caveinfo/my_arena.txt
    archives/                             files inside archives
      enemy/parm/enemyParms.szs/kochappy/enemyparm.txt
```

- Each mod is one folder. The folder name must be the mod's id.
- Folders whose name starts with a dot are ignored. `.cache` is reserved.
- At most 256 mod folders are read.
- A mod folder may be a link to a folder elsewhere. Nothing inside a mod may be a link.

## 3. mod.json

`mod.json` is a JSON object in UTF-8. It may be at most 64 KiB and nested at most 16 levels deep.

| Key | Required | Rule |
|---|---|---|
| `id` | yes | 1 to 64 characters of `a-z`, `0-9` and `-`. Must equal the folder name. |
| `version` | yes | `MAJOR.MINOR.PATCH`, for example `1.2.0`. An optional `-pre` or `+build` part may follow. At most 32 characters. |
| `files` | yes | A list of 1 to 4096 paths. Every file the mod ships must be listed. |
| `name` | no | The name shown in the launcher, 1 to 64 characters. Default: the id. |
| `mppm` | no | The oldest Moonsprout the mod works with: `>=0.1`, `>=0.1.0` or `>=1`. |
| `author` | no | At most 64 characters. |
| `description` | no | At most 1024 characters. |
| `affects` | no | `"gameplay"` (default) or `"cosmetic"`. Kept for online play: later, cosmetic mods may stay local. |

Rules:

- A key with the wrong type, or a string that is too long, is an error.
- A key that Moonsprout does not know is a warning. It is ignored.
- Files in the mod folder that are not listed in `files` are ignored.
- A listed file that is missing is an error.
- If `mppm` asks for a newer Moonsprout, the mod does not load ("Needs Moonsprout X.Y or newer.").

## 4. Paths, keys and case

Every entry in `files` is a path relative to the mod folder, with `/` between the parts.

- It starts with `files/` (a loose file) or `archives/` (a file inside an archive).
- It is at most 240 bytes and has at most 16 parts.
- Each part is 1 to 64 characters of `A-Z`, `a-z`, `0-9`, `.`, `_` and `-`.
- A part may not start or end with a dot. So `.` and `..` are not allowed.
- A part may not be a Windows device name: `con`, `prn`, `aux`, `nul`, `com1` to `com9` and
  `lpt1` to `lpt9`, also with an extension such as `con.txt`.
- No backslash, no colon, no empty part (`//`), no `/` at the start or the end.
- The file must be a regular file inside the mod folder, not a link.

Moonsprout turns each path into a **key**. Keys are lower case, so case does not matter:
`files/user/Abe/vs/stages.txt` and `files/user/abe/vs/stages.txt` are the same file.

| Path in mod.json | Key |
|---|---|
| `files/user/Abe/vs/stages.txt` | `/user/abe/vs/stages.txt` |
| `archives/enemy/parm/enemyParms.szs/kochappy/enemyparm.txt` | `/enemy/parm/enemyparms.szs:kochappy/enemyparm.txt` |

Two entries of one mod may not have the same key.

## 5. Files inside archives

Put the file under `archives/<archive path>/<path inside the archive>`.

- The archive path is the disc path of the `.szs` or `.arc` file. It ends at the first part that
  ends in `.szs` or `.arc`.
- The path inside the archive starts below the archive's root folder. Leave the root folder's own
  name out. Case does not matter.
- The archive may be Yaz0-compressed on the disc. Your file is plain (not compressed).

What works in this version:

- **Replacing a file that the archive already holds.** This is the only kind of change.
- Adding a file to an archive is **not supported yet**. The mod fails with "…has no file X.
  Adding files to an archive is not supported yet."
- Archives inside archives are **not supported yet**. A path such as
  `archives/outer.szs/inner.arc/file.txt` fails with "The file inner.arc in the archive /outer.szs
  is itself an archive. Archives inside archives are not supported yet; replace the whole
  /outer.szs as a loose file instead."
- To change many files of one archive, or to add files to it, you can replace the whole `.szs` as
  a loose file instead (`files/enemy/parm/enemyParms.szs`). It must start with `Yaz0` or `RARC`.
- A whole-archive replacement and inner-file overrides of the same archive follow the load order,
  like any other conflict (section 6). The mod that replaces the whole archive counts as changing
  every file inside it:
  - An inner file from a mod **lower** in the list (higher priority) than the archive's mod goes on
    top of the replaced archive. The Mods window shows "**X overrides Y**: enemy/parm/enemyparms.szs:kochappy/enemyparm.txt
    (on top of the whole archive)", where Y replaced the archive.
  - An inner file from a mod **higher** in the list (lower priority) is not used: the replaced
    archive is used as it is. The Mods window shows "**Y overrides X**: …:kochappy/enemyparm.txt
    (whole archive)".
  - One mod may replace an archive and also list inner files of it; its inner files go on top of
    its own archive.

## 6. Load order and conflicts

`load_order.txt` lists the mods that are on, one id per line:

```
# Moonsprout mod load order: top = lowest priority, bottom = highest. "-" turns a mod off.
my-arena
-old-test
better-caves
```

- `<id>` turns a mod on. `-<id>` turns it off.
- The top line has the lowest priority. The bottom line has the highest.
- Empty lines and lines that start with `#` are skipped.
- A mod folder that is not listed is **New**. It is off until you turn it on.
- The launcher's Mods window edits this file for you. When it writes the file, your comments are
  not kept.

When two mods that are on change the same file (the same key), the mod lower in the list wins.
The launcher shows this as "**X overrides Y**: user/abe/vs/stages.txt". File contents are never
merged: the whole file comes from the winner.

## 7. Checks and limits

Moonsprout checks some known files before it loads a mod.

| File | Check | Kind |
|---|---|---|
| `user/Abe/vs/stages.txt` | The first number equals the number of `{ }` blocks, from 1 to 10. The stage select shows at most 10. | Error |
| `user/Matoba/challenge/stages.txt`, `kfes-stages.txt` | The same count, at least 30. | Error |
| Any loose `.szs` or `.arc` | Starts with `Yaz0` or `RARC`. | Error |
| `.txt` and `.ini` | No NUL byte; `{` and `}` pair up (after `#` comments are removed). | Warning |

Limits:

| What | Limit |
|---|---|
| One file | 64 MiB |
| One file inside an archive | 16 MiB |
| One mod | 256 MiB |
| All mods that are on, together | 512 MiB |

The checks do not read every format. A broken cave file can still stop the game. Test your mod.

## 8. Problems

A mod with any **error** does not load at all. Moonsprout never loads half a mod. The launcher shows
the mod as **Problem** and lists each error with the file it is about. Other mods still load.

A **warning** does not stop the mod. Read it anyway.

Some problems are found only when the game starts, because they need the disc. For example: the
archive is not on the disc, or the archive has no such file. The mod is then dropped and the
other mods load without it. The Mods window shows these problems too.

## 9. The content hash

Moonsprout computes two hashes of the mods that are on:

- The **mods hash** covers each mod that is on, in load order: its id, its version, and for every
  listed file its key, size and SHA-256. With no mod on it is all zeros.
- The **content hash** covers this build of Moonsprout and the same mod data.

Load order is part of both hashes, because it decides which file wins. A file that another mod
overrides still counts.

Online, everyone in a game needs the same content hash. That means the same Moonsprout build and the
same mods, in the same order. The game runs in lockstep: each player's game simulates the same
match from the same inputs. Different data would make the games drift apart.

Replays store the mods hash and the list of mods. A replay that was recorded with other mods does
not play. Turn on the same mods in the same order. (A replay recorded on another build plays with a
warning, as before.)

The Mods window shows the content hash. With no mods on, it covers only the build, so players with
the same build match.

## 10. Testing a mod

- Start Moonsprout with another mods folder: `--mods-dir DIR` or the environment variable
  `MPPM_MODS_DIR=DIR`. The flag wins.
- Start without any mods: `--no-mods` or `MPPM_NO_MODS=1`.
- Mods load when the game starts. Changes in the start menu apply on the next start. If the game
  is already running behind the menu, **Play** restarts Moonsprout first.

The game prints its mod work to the console (stderr):

```
mppm: mods: folder <dir>: 2 on (my-arena@1.0.0, better-caves@0.3.0), 3 files, 1 archive files, 1 conflicts
mppm: mods: conflict /user/abe/vs/stages.txt: better-caves overrides my-arena
mppm: mods: conflict /enemy/parm/enemyparms.szs:kochappy/enemyparm.txt: big-enemies overrides my-tuning (big-enemies replaces the whole archive)
mppm: mods: error <id>: <path in mod.json>: <message>
mppm: mods: warning <id>: <path in mod.json>: <message>
mppm: mods: skipped <id> (errors above)
mppm: mods: hash <mods hash> content <content hash>
mppm: mods: archive override /enemy/parm/enemyparms.szs:kochappy/enemyparm.txt applied (mem, <n> bytes)
```

With no mods on, nothing is printed, unless you gave a folder with `--mods-dir` or
`MPPM_MODS_DIR` ("folder <dir>: none on"). With mods on, two problems of mods that are off are
printed too, as warnings: a folder whose name is not a mod id (it can never load: rename the folder
to the mod's id), and a `-<id>` line in `load_order.txt` whose folder is missing.

`tools/mods/modhash.py` checks a mods folder without starting the game. It prints the same kind of
lines and the mods hash. Run it with `python3 -I tools/mods/modhash.py <mods folder>`.

## 11. Example

A mod that changes the time limit of the first 2P Battle stage.

1. Copy `files/user/Abe/vs/stages.txt` from your disc (with
   `dtk vfs cp "<your disc>:files/user/Abe/vs/stages.txt" .`, or any GameCube disc tool).
2. Open it in a text editor that keeps the file's encoding (Shift-JIS) and line ends. Change the
   `time` number of the first stage. Keep the 10 stages.
3. Make this folder:

```
mods/
  load_order.txt
  short-battles/
    mod.json
    files/user/Abe/vs/stages.txt
```

`mod.json`:

```json
{
  "id": "short-battles",
  "version": "1.0.0",
  "name": "Short battles",
  "mppm": ">=0.1",
  "author": "you",
  "description": "The first Battle stage has a shorter timer.",
  "files": ["files/user/Abe/vs/stages.txt"]
}
```

`load_order.txt`:

```
short-battles
```

4. Start Moonsprout. In the Mods window's list, the mod's entry shows its name, "Short battles",
   with the state "On" on a small line below it. The console shows
   `mppm: mods: folder …: 1 on (short-battles@1.0.0)`.

To share the mod, share the `short-battles` folder. Tell players to put it in their mods folder
and turn it on in the Mods window.
