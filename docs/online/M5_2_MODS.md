# M5.2: data-mod overlay, manifest and content hash

Milestone M5.2 of [`docs/ONLINE_MULTIPLAYER.md`](../ONLINE_MULTIPLAYER.md) (section 8, "Step 2";
design in 7a, the replay part of 7b). Mods replace the game's data files: loose disc files through
Aurora's DVD overlay, and files inside `.szs`/`.arc` archives through a splice in the RARC table
conversion. A manifest, a load order and validators decide what is mounted. Two hashes describe the
result: the mods hash gates replays now; the content hash is for the online handshake (M5.3b).

The guide for mod authors is [`docs/mods/README.md`](../mods/README.md). The replay byte layout is
in [`M5_1A_HARNESS.md`](M5_1A_HARNESS.md).

There is no edit in `src/` or `include/`. With no mods folder, no `load_order.txt` or no mod on,
the game behaves as before: no extra work at start-up, no log line, the same checksum logs (the
golden logs in `tools/net/golden/` still match) and a content hash that covers only the build.

## 1. Code map

| Part | Where |
|---|---|
| C interface (`p2_mods_init`, `p2_mods_mount`, `p2_mods_count`, `p2_mods_dir`, `p2_mods_content_hash`, `p2_mods_arc_*`) | `port/include/p2_mods.h` |
| C++ view for the launcher (scan, states, issues, `write_load_order`, `content_hash`, `mounted()`) | `port/include/p2_mods.hpp` |
| Scan, manifest, load order, validators, hashes, loose overlay, mount | `port/mods/` (pure, testable parts in `port/mods/pure/`, among them a FIPS 180-4 SHA-256) |
| Archive index, registry, `check`/`install` | `port/mods/pure/archive_index.*`, `port/mods/archive_mount.cpp`, interface `port/mods/archive.hpp` |
| Splice | `port/assets/rarc.cpp`, inside `JKRArchive::p2HostTables` |
| `.mpnl` v3, content gate, build id | `port/net/net.cpp`, `port/include/p2_net.h` (M5.2 block), `tools/net/netlog.py` |
| Mods window, menu entries, pending restart, version label | `port/launcher/mods_window.*`, `res/rml/mods.rcss`, `integration.cpp`, `restart.cpp`, `prelaunch.cpp` |
| Self-test | `tests/mods/` (target `p2mods_selftest`) |
| Test tools | `tools/mods/make_test_mods.py`, `tools/mods/modhash.py`, `tools/mods/mods_test.sh` |

`port/main.cpp` calls `p2_mods_init(argc, argv)` after `p2_net_init` and `p2_mods_mount()` after a
successful `aurora_dvd_open` (`port/main.cpp:945`), before the start menu. `port/sdk/dvd_fixes.cpp`
is unchanged: the overlay sits below `DVDOpen` and `DVDConvertPathToEntrynum` inside Aurora
(`dvd.cpp:1102-1108`, `:1081`), so the existing `p2_` wrappers reach it.

## 2. Folder, flags and environment

The mods folder is `<data>/mods`, where `<data>` is `SDL_GetPrefPath(NULL, "mppm")` (the launcher's
data folder, for example `~/.local/share/mppm/`).

| Flag | Environment | Effect |
|---|---|---|
| `--mods-dir DIR`, `--mods-dir=DIR` | `MPPM_MODS_DIR=DIR` | use DIR as the mods folder |
| `--no-mods` | `MPPM_NO_MODS=1` | mount nothing (stock content) |

The flag wins over the environment. The launcher ignores these arguments, and a restart re-executes
the same command line, so they persist across restarts.

## 3. Mount sequence

`p2_mods_mount()` runs once, right after the disc is open (DVD reads work then: `aurora_dvd_open`
starts the worker, `dvd.cpp:650-707`).

1. `--no-mods`: report stock content to the net layer and return. Folder missing, no
   `load_order.txt`, or nothing on: the same, silently. Only when the folder came from
   `--mods-dir` or `MPPM_MODS_DIR` one line says `mppm: mods: folder <dir>: none on`.
2. Scan (section 4): manifests, load order, file hashes and the checks that need no disc. Its
   mods hash is kept as `mounted().folderHash` (the restart rule, section 9). The issues of the
   enabled mods and the folder are logged; of the mods that are off, only two are: a folder whose
   name is not a mod id ("The folder name is not a mod id…", a warning; it can never be listed or
   load) and a `-<id>` line whose folder is missing (`pure::off_notices`). With no mod on nothing
   is logged, as before.
3. Round loop, at most one round per mod plus one (each round that does not finish drops at least
   one mod). The mounted set is the enabled valid mods in load order.
   - Read every listed file of the set into a 32-byte aligned, port-owned blob padded with 32
     zero bytes, and hash it again. A file that changed since the scan fails its mod ("The file
     changed while the game was starting.").
   - Winners (`pure::pick_winners`): the last mod wins per key, and a mod that replaces a whole
     archive (loose key `<arc>`) beats the inner keys `<arc>:<inner>` of the mods before it, whose
     files are then not used (README 5; the inner files of a mod after it go on top of the
     replaced archive). Loose winners (keys without `:`) go to Aurora with
     `aurora_dvd_overlay_callbacks` (once) and `aurora_dvd_overlay_files`: `fileName` is the key,
     or the disc's own spelling of it when the disc has that file (`DVDConvertPathToEntrynum`, then
     `DVDConvertEntrynumToPath`), `userData` the blob. An entry number of -1 back fails that mod ("The game could not use this
     path: a folder on the disc has that name.").
   - Archive winners (keys with `:`) go to `archive::check`. A problem fails that entry's mod.
   - Any failure drops the failed mods (logged) and starts the next round. Otherwise
     `archive::install` and stop.
4. Serialize the mounted set (section 5) and compute the mods hash. With mods on, also compute
   the content hash (this reads the executable once) and log the summary.
5. `p2_net_set_content(modsHash, list)`, always (stock: zeros and "").

The overlay callbacks serve from memory: `open` makes a cursor `{blob, pos}`, `read` copies
`min(len, size - pos)`, `seek` uses whence 0/1/2 and refuses a position outside `[0, size]`,
`close` frees the cursor. The blobs are never freed (M5.2 mounts once). Reading every winning file
at mount means the bytes served are exactly the bytes hashed, and the callbacks are thread-safe.

A replaced disc file keeps its entry number; a new file gets a fresh one. Lookups are
case-insensitive (`fst.cpp:293-311`), so `/user/abe/vs` and `/user/Abe/vs` find the same file.
Aurora gives a replaced FST entry the overlay's name, and `JKRFileCache::findFile` compares
`DVDReadDir` names case-sensitively (`JKRFileCache.cpp:365-385`; the boot loads
`/AudioRes/PSound.aaf` that way, `system.cpp:577`), so a replacement is registered under the disc's
spelling: registered as `/audiores/psound.aaf` it hid the file and the boot asserted (fix round 2;
the identity test mod now replaces `PSound.aaf`). A new file keeps its lower-case key.

## 4. Formats

The author-facing rules are in [`docs/mods/README.md`](../mods/README.md). In short:

- **Folder.** One folder per mod, named after its id; entries starting with `.` are ignored; at most
  256 folders (sorted by name). A mod folder may be a symlink; nothing inside a mod may be one.
- **`mod.json`.** UTF-8 object, at most 64 KiB, depth at most 16 (a prescan of `[`/`{` outside
  strings runs before the parser, which is nlohmann_json in non-throwing mode). Keys `id`
  (`^[a-z0-9-]{1,64}$`, equal to the folder name), `version` (semver, at most 32 characters), `files`
  (1..4096 paths), and optional `name`, `mppm` (`>=X[.Y[.Z]]`, compared with the numeric prefix of
  `MPPM_VERSION_STRING`), `author`, `description`, `affects` (`gameplay`/`cosmetic`). Unknown keys
  are a warning.
- **`load_order.txt`.** At most 64 KiB and 1024 lines; `<id>` on, `-<id>` off, `#` comments; top =
  lowest priority. A folder not listed is "new" and off. Malformed or repeated lines and listed ids
  without a folder are folder warnings. The launcher rewrites the file with one header comment and
  does not keep other comments.
- **Paths and keys.** At most 240 bytes and 16 parts of 1..64 bytes of `[A-Za-z0-9._-]`; no part
  starts or ends with `.`; no Windows device names; no `\`, `:`, empty part or leading `/`; first
  part `files` or `archives`. On disk a regular file, not a symlink, inside the canonical mod
  folder. `files/<p>` → key `/` + lower(p); `archives/<a>/<i>` → `/` + lower(a) + `:` + lower(i),
  where `<a>` ends at the first part ending in `.szs` or `.arc`.
- **Limits.** File 64 MiB, archive inner file 16 MiB, one mod 256 MiB, all enabled mods 512 MiB.
- **Validators.** VS `stages.txt`: first number = number of top-level `{}` blocks, 1..10 (Error).
  Challenge `stages.txt` and `kfes-stages.txt`: the same, at least 30 (Error). A loose `.szs`/`.arc`
  starts with `Yaz0` or `RARC` (Error). `.txt`/`.ini`: no NUL, balanced braces (Warning).
- **Failure unit.** A mod with any error is not mounted at all.

## 5. Hashes

`S` is, for each mounted mod in load order: `u32le(len id) id u32le(len version) version
u32le(file count)`, then for each listed file in bytewise key order: `u32le(len key) key u64le(size)
SHA-256(bytes)`. Every listed file counts, also one that another mod overrides.

```
mods_hash    = SHA-256("mppm-mods-v1" ‖ S)                       32 zero bytes when no mod is mounted
content_hash = SHA-256("mppm-content-v1" ‖ u64le(build_id) ‖ S)  stock: the build alone
build_id     = p2_net_build_id()  (FNV-1a 64 of the executable)
```

**Test vector** (checked by `p2mods_selftest` and `python3 -I tools/mods/modhash.py --selftest`):
build id `0x0123456789abcdef`; `alpha` `1.0.0` with `/user/abe/vs/stages.txt` = `b"10\n"`, then
`beta` `0.2.1` with `/user/mukki/mapunits/caveinfo/test.txt` = `b"hello\n"` and
`/enemy/parm/enemyparms.szs:chappy/enemyparm.txt` = `b"x"`. `len(S) = 283`.

| Value | SHA-256 |
|---|---|
| content | `699dce5678b94e4d309afa06d1e0de3c8c213ae8b1788d05cfc8ff3a07fd6ac3` |
| mods | `d54c604f3bb5fb1d3f40e5b0cd870aa43c26f9c1a0ecaee2b490e16eaf7c531c` |
| stock content (same build id) | `553c9181fbcecab7b9b7d1ce992639878b12c35427e39fd5b92ddc2ee8d1c001` |
| content, order beta then alpha | `fe7fbbbee89acbfb88b3f51ec383c06bf898775289949e123583ccdb5ac48e53` |
| mods, order beta then alpha | `9fd19ce6bdc8c5be6150849c3323c21900ef97535807c99aa92916ac003748ff` |

The content hash needs the build id, which hashes the 165 MB executable (about 0.2 s). It is
computed only with mods on, for a recording, or by the Mods window (on a worker thread).

## 6. In-archive overrides (v1: replace)

**Index.** `yaz0_decode` is bounded (output at most 64 MiB; a back reference before the start or a
source overrun is an error). `rarc_index` checks the `RARC` magic, the header length and every table
offset and count against the data (at most 65535 folders, 1<<20 entries, depth 32, each folder
visited once), walks from folder 0 skipping `.`/`..` and maps lower(inner path) → file index.

**Archive key.** FNV-1a 64 over the raw info block (`SArcDataInfo`, disc byte order) from offset 0
to `mStrTableOffset + mStrTableLength`. It is computed from the decoded disc archive at install and
on entry to `p2HostTables` before the swap. Buffer mounts (`enemyParms.szs`, loaded through ARAM
and mounted with `JKRMountArchive(void*)`, `generalEnemyMgr.cpp:775-779` → `loadResource.cpp:154`,
`:200-201`) have no path, but the info block holds every name, size and offset, so equal keys mean
the same layout. Path mounts break ties with `mEntryNum`. Two targeted archives with identical
tables log a warning; none of the stock targets do.

**check / install.** Each distinct archive is read once through Aurora (`DVDOpen`, `DVDReadPrio`,
`DVDClose`, after the loose overlay, so a whole-archive replacement is the base) and Yaz0-decoded.
Only inner files that win over the archive's whole replacement reach `check` (section 3), so an
inner file of a mod before the replacement is neither checked nor applied.
Problems: archive not on the disc; not an archive; inner file missing ("…has no file <inner>.
Adding files to an archive is not supported yet."); inner path is a folder; more than 254
archives. `install` gives each archive set an id 1..254 and stores its key and entry number; the
registry is immutable afterwards and read without locks.

**Splice** (`port/assets/rarc.cpp`, `p2HostTables`). Before the swap, when overrides are
installed, the set is found by the raw info block's key. After the swap the set id is written to
the info block's padding byte `_1B[3]` (offset 0x1E), next to the existing conversion mark
`_1B[4]`, so a re-mount of an already converted buffer finds its set again without a pointer map.
After the existing copy loop, each override entry (never a folder) gets `mData` = the mod bytes,
`mSize` = their length, and the compressed and Yaz0 flags cleared. Every `fetchResource` returns a
preset `mData` as is, and `getResSize` returns `mSize`. The missing null check after the host
table allocation is added (abort with `mppm: rarc: no memory for the file table (N entries)`). With
no override installed, `p2HostTables` writes no byte it did not write before.

**Memory rules.**

| Mount mode | `mData` of an override |
|---|---|
| `EMM_Mem` (every game mount; all 57 `JKRMountArchive` calls in `src/` outside JSystem) | the port-owned blob, shared between mounts |
| `EMM_Dvd`, `EMM_Aram`, `EMM_Comp` (JAudio) | a copy from the archive's heap, because their destructors free `mData` there; for Comp, flag 0x10 is cleared so its destructor frees the copy |

A failed copy leaves the entry stock and logs `failed (<mode>): out of memory`.

**Memory cost of the Dvd/Aram/Comp copies.** The copy is made when `p2HostTables` runs, at the
archive's mount, for every overridden file of that archive, whether or not the game ever fetches
it: each costs its size rounded up to 32 bytes plus 32 from the archive's heap until the archive is
unmounted (a stock `EMM_Dvd` archive allocates a file only when it is fetched). With the limits,
that is at most 16 MiB + 64 bytes per file. It stays eager because a lazy copy would need a hook in
the archives' `fetchResource` (`JKRDvdArchive.cpp`, `JKRAramArchive.cpp`, `JKRCompArchive.cpp` in
`src/`, which M5.2 does not edit): with `mData` left null, `fetchResource` reads the disc file's
bytes at `mDataOffset`, not the mod's. Only JAudio mounts archives in these modes, and no test or
known mod targets them (section 13). Shared blobs are
safe because no game code calls `removeResource`/`removeResourceAll`, and the port's in-place
converters (`j3d_model.cpp`, `j3d_anim.cpp`, `jpa.cpp`, `jstudio_fvb.cpp`, `res_font.cpp`) are
idempotent (marks, or the big-endian tag for fonts). **A new in-place converter without a mark would convert a re-mounted shared override twice.**

## 7. Replays: `.mpnl` version 3 and the content gate

The engine writes version 3 and reads versions 2 and 3. Version 3 is the version 2 header (64
bytes) plus the mods hash (32 bytes at 64), the content hash (32 bytes at 96, informational) and the
mod list `id@version,…` (NUL-terminated, padded to a multiple of 16, 16..1024 bytes at 128). The
header size at offset 6 covers it. The byte layout is in
[`M5_1A_HARNESS.md`](M5_1A_HARNESS.md). A version 2 log means stock content.

The gate (in `p2_net_set_content`):

- A replay whose **mods hash** differs from this run's is refused with exit code 2:
  `mppm: net: the replay was recorded with other content: replay has <list|none> (mods <hash8>),
  this run has <list|none> (mods <hash8>); turn on the same mods in the same order`.
- A different build id stays a warning, as before, so the determinism corpus can be replayed
  across builds.
- A synthetic replay (header flags bit 0) is not checked; with mods on, one line says
  `mppm: net: synthetic replay: content not checked (this run: <list>)`.
- With mods on, the checksum log gains one comment after `# build=`: `# mods=<hex> <list>`. For
  stock content the checksum log is unchanged, and `netlog.py cmp-cksum` ignores comments.

`netlog.py`: reads versions 1 to 3; `dump` prints the mods hash, content hash and list; `synth
--format-version 3` is the default, with `--mods-hash` and `--mods-list`; `desync_ci.sh` keeps
passing `--format-version 2`.

## 8. Log lines

```
mppm: mods: folder <dir>: <n> on (<id@ver>, ...), <f> files, <a> archive files, <c> conflicts
mppm: mods: folder <dir>: none on                (only for a folder given by flag or environment)
mppm: mods: conflict <key>: <winner> overrides <loser>[, <loser>...][ (<id> replaces the whole archive)]
mppm: mods: error <id>: <manifest path or "-">: <message>
mppm: mods: warning <id>: <manifest path or "-">: <message>
                                                 (also, with mods on, for a mod that is off: a
                                                 folder name that is not an id, a missing folder)
mppm: mods: skipped <id> (errors above)
mppm: mods: hash <64 hex> content <64 hex>
mppm: mods: archive override <archive>:<inner> applied (<mem|aram|dvd|comp>, <n> bytes)   (first time per file)
mppm: mods: archive override <archive>:<inner> failed (<mode>): out of memory
mppm: mods: warning: archives <a> and <b> have identical tables; buffer mounts use the first
mppm: net: content: <n> mods <hash8> (<list>)
mppm: net: synthetic replay: content not checked (this run: <list>)
mppm: net: the replay was recorded with other content: ...
```

## 9. Launcher: the Mods window

Start menu order: **Play, Mods, Settings, Quit**. The window follows the Save Manager's shell
(Controlled list pane, Uncontrolled detail pane, `saves.rcss` tokens, the same keyboard and
controller navigation and footer prompts).

- **List.** "Overview", then a "Mods" header with an **Open Mods Folder** icon button, then one
  entry per mod in load order: the name across the row and the state word (`On|Off|New|Problem|
  Missing`) on a small second line, so similar names stay apart. Empty folder: "No mods yet."
- **Reading.** The folder scan (every listed file of the mods that are on read and hashed, and of
  the mods that are off up to 512 MiB in all; beyond that an off mod is checked by name and size)
  and the content hash run on a worker thread, when the window opens and after each change; meanwhile the window
  shows the last scan, "…" for the hash, and disabled load-order buttons ("Reading the mods
  folder…"). The worker is a detached thread that nothing joins, so Quit never waits for it; it
  gets the folder from `mods::scan_args()` and touches no other module state.
- **Overview.** "<n> on · content <8 hex>" (or "Stock content · <8 hex>"; "…" while the folder is
  read), the full 64-digit content hash with "Everyone in an online game
  needs the same content hash…", the folder path and folder issues.
- **Mod detail.** Name, version, id and author; the state line; file count and size; "Load order"
  with **Turn On/Turn Off**, **Move Up**, **Move Down** (disabled at the ends, for invalid, missing
  or locked mods; a missing mod gets **Remove From List** instead of Turn On/Off, and its warning is
  not repeated under Problems; a move swaps with the nearest mod whose folder exists; disabled buttons still take the focus so the note can explain); "Conflicts" ("X
  overrides Y: <key>", at most 20; for a file inside an archive another mod replaces as a whole,
  " (whole archive)" or " (on top of the whole archive)" follows); "Problems" as help notes with
  their manifest path (a folder whose name is not an id has its warning there); "Files" with
  **Open Mod Folder**. Selecting another item scrolls the detail pane back to its top.
- **Rules.** Every change writes `load_order.txt` at once. Before the game boots nothing is
  pending. With the game booted behind the live menu, a folder whose mods hash differs from
  `mounted().folderHash` (the folder as the boot read it, before the mount dropped any mod) makes
  **Play** restart Moonsprout first (counted with the Save Manager's pending restarts): a mod the
  mount dropped for a disc-dependent reason is no change, while editing, updating, reordering or
  toggling any mod is. `mods_restart_pending()` waits only for a scan that follows a load-order
  change made in the window; otherwise the last finished scan answers at once. In game
  (F1 bar): read-only, "Return to the start menu to change mods."; the count and the content hash
  are the running game's (`p2_mods_count`, and `p2_mods_content_hash` computed on the scan's
  worker, "…" until ready, because the first call reads the executable), the list is the folder now, and
  a folder changed since the boot adds "The mods folder has changed since the game started…". During a replay or harness
  session: read-only, locked. With `--no-mods`: "Mods are off for this run (--no-mods)."
- **Version label.** The start menu's version label gains " · N mod(s)" when mods are mounted.

Details are in [`docs/launcher/SETTINGS.md`](../launcher/SETTINGS.md), "Mods window".

## 10. Test tools

- **`tools/mods/make_test_mods.py --disc D --out DIR`** builds the test mods from your own disc
  (USA, GPVE01). It reads plain ISO/GCM images itself (FST at 0x424/0x428, Yaz0, RARC) and falls
  back to `dtk vfs cp` (`$MPPM_DTK`, `build/tools/dtk`, PATH) for other images. Edits are
  text-level and keep the Shift-JIS bytes and CRLF line ends. It refuses an `--out` inside the
  repository; nothing it writes may be committed. It writes the scenario folders `stock-empty`,
  `arena`, `conflict`, `conflict-rev`, `archive`, `enemy`, `pellet`, `pellet-bmd`, `identity`,
  `bad` (which also holds a folder named `Bad Folder Name` and a `-mppm-test-gone` line with no
  folder), `archive-whole` (enemyparms, then enemyparms-whole) and `archive-whole-rev` (the other
  order), each with its own `load_order.txt`, and `scenarios.json` with the expectations.

  | Mod | Content |
  |---|---|
  | `mppm-test-vs-arena` | `stages.txt` stage 0: `col0 happa0` and `col1 happa0` 3 → 10, time 200 → 120; `vs_1_otegaru.txt` `{f002}` 30 → 6 |
  | `mppm-test-vs-newfiles` | `stages.txt` stage 0 names two new files: `mppm_test_arena.txt` (caveinfo copy, `{f002}` → 12, Pelplant weight 100 → 10, Kochappy weight 40 → 4, so the enemy count differs from arena's) and `mppm_test_arena_edit.txt` (edit layout copy) |
  | `mppm-test-enemyparms` | `enemyParms.szs`: `kochappy/`, `egg/`, `pelplant/` `enemyparm.txt` `{fp00}` × 4 |
  | `mppm-test-pelletlist` | `pelletlist_us.szs`: `numberpellet_config.txt` `number1`, `number5` radius, p_radius, height × 3, max + 2 |
  | `mppm-test-pelletlist-bmd` | as above, and `number1` uses `white2.bmd` (the screenshot fallback) |
  | `mppm-test-enemyparms-whole` | the disc's own `enemyParms.szs` as a loose file (a whole-archive replacement that changes nothing) |
  | `mppm-test-identity` | the disc's own `stages.txt`, `AudioRes/PSound.aaf` (read case-sensitively at boot), `kochappy/enemyparm.txt`, `numberpellet_config.txt` |
  | `mppm-test-bad-*` | hand-written, no game data: malformed JSON; `../` path; absolute path; symlink to `/etc/hostname`; folder name ≠ id; listed file missing; `"mppm": ">=99.0"`; an 11-stage synthetic VS list; a 70 MiB sparse file; 10 000 nested `[` |

- **`tools/mods/modhash.py MODS_DIR [--build-id HEX | --exe PATH] [--json FILE]`** is the Python
  reference of sections 4 and 5: it scans a folder and prints lines in the form of the game's log,
  including `mppm: mods: hash <64> content <64>`, which matches the game's on a valid set (check
  3). Its error sentences are not all worded like the game's. It makes no disc-dependent checks:
  a mod the game drops at the mount (an archive that is not on the disc, an inner file the archive
  lacks, a loose path that is a disc folder) still counts here, so on such a folder the hashes
  differ from the game's. `--selftest` checks the test vector and the path rules.
- **`tools/mods/mods_test.sh`** is the done test (section 11).

## 11. Done test

```sh
WT=<integration worktree>; cd "$WT"
git -C extern/aurora checkout -- .
distrobox enter mppm-build -- bash -lc "cd $WT && cmake build-pc >/dev/null && cmake build-pc >/dev/null && ninja -C build-pc mppm p2mods_selftest"
./build-pc/p2mods_selftest                       # "p2mods_selftest: N passed, 0 failed", exit 0
python3 tools/net/netlog.py selftest             # exit 0
python3 -I tools/mods/modhash.py --selftest      # exit 0
MPPM_TEST_DISC=<your disc> tools/mods/mods_test.sh -d 85 -o /var/tmp/mppm-m52-done
MPPM_TEST_DISC=<your disc> tools/net/desync_ci.sh --quick -d 86 -o /var/tmp/mppm-m52-desync
git grep -n "TEMPORARY M5.2 ""STUB"              # no output (the quotes keep this line out of it)
git diff --stat online/m5.2 -- src include       # no output (no upstream edit)
```

`mods_test.sh [-b BUILD] [-d DISPLAY] [-o OUT] [-c CHECK,...] [-l]` builds the test mods into
`OUT/mods-test`, synthesizes `Ls` (`netlog.py synth --seed 20261122 --synth-seed 7 --vs-stage 0
--ticks 3600`, the log of `tools/net/golden/Ls.cksum`) and replays it headless under Xvfb with an
isolated `HOME`/`XDG_*` per run. Each check prints its findings and one `PASS`/`FAIL` line; the
script exits 0 only if every check passed. Logs, checksum logs and PNGs stay in `OUT`.

| # | Check | Passes when |
|---|---|---|
| 1 | `stock` | no mods folder: `cmp-cksum` with golden `Ls` identical; no `mppm: mods:` line. Then `--no-mods` with `MPPM_MODS_DIR` = the arena folder: the same |
| 2 | `stock-empty` | `MPPM_MODS_DIR` = a folder without `load_order.txt`: golden; only the `none on` line |
| 3 | `identity` | golden C lines; `applied (mem` for `enemyparms.szs` and `pelletlist_us.szs`; `# mods=` in the checksum log; the engine's hash line equals `modhash.py` with the run's build id |
| 4 | `arena` | two runs `cmp`-identical; the first C line differs from golden in `n=`; the tick-60 picture (`MPPM_HOLD_AT_TICK=60`, paced) differs from stock by more than 900 pixels (`magick compare -fuzz 15% -metric AE`) outside the stock-to-stock noise: the pixels where the two stock pictures differ, grown by 4, are painted out of all three pictures and the smaller count against either stock picture is used |
| 5 | `enemy` | two runs identical; differs from golden with `enemy` among the fields |
| 6 | `pellet` | `applied (mem` for `pelletlist_us.szs`; differs from golden, or else the `pellet-bmd` picture at tick 60 differs from stock (the evidence used is printed and written to `OUT/pellet_evidence.txt`) |
| 7 | `archive` | two runs identical; differs from golden |
| 8 | `conflict` | `conflict /user/abe/vs/stages.txt: mppm-test-vs-newfiles overrides mppm-test-vs-arena`; first C line's enemy count differs from arena's; `conflict-rev`: the reverse line and C lines equal to arena's |
| 9 | `bad` | per bad mod one `error` line with its path and message fragment from `scenarios.json`, and one `skipped`; exit 0; the arena mod mounted (`n=` as in check 4); the two warnings for `Bad Folder Name` (not an id) and `mppm-test-gone` (an off line with no folder) |
| 10 | `record` | `--record` with arena: `netlog.py dump` shows version 3, the arena mods hash and `mppm-test-vs-arena@1.0.0` |
| 11 | `refuse` | the recording without mods and with `identity`: exit 2 and the `recorded with other content` line; with arena: exit 0 and C lines equal to check 4 |
| 12 | `launcher` | `MPPM_MODS_DIR` = a copy of `conflict`, a config with `backend.discPath` and `interface.presetChosen`, no `MPPM_SKIP_LAUNCHER`: xdotool Down to Mods, Return (`mods_overview.png`), Down to the second mod (`mods_detail.png`), Right and Return on Turn Off (`load_order.txt` gets `-mppm-test-vs-newfiles`; `mods_pending.png` should show the pending-restart note), Move Up (newfiles above arena), Escape twice (detail pane to list, then close: `start_menu.png`, focus on Mods), Quit; screenshots non-blank and different |
| 13 | `whole` | `archive-whole`: golden C lines (the inner files before the whole-archive replacement are not used), one `conflict … mppm-test-enemyparms-whole overrides mppm-test-enemyparms (mppm-test-enemyparms-whole replaces the whole archive)` line per inner file, no `applied` line for `enemyparms.szs`; `archive-whole-rev`: the reverse conflict lines, `applied (mem`, C lines equal to check 5's |

The launcher check's key counts can be adjusted without editing the script:
`MPPM_MODS_UI_LIST_DOWNS` (default 3: Overview → Open Mods Folder → first mod → second mod),
`MPPM_MODS_UI_OFF_KEYS` (default none) and `MPPM_MODS_UI_UP_KEYS` (default `Down`).

### Results

Recorded after fix round 1 (branch `online/m52-int`, 2026-11-23, USA disc, Xvfb display 91):

- `./build-pc/p2mods_selftest`: 19 passed, 0 failed (core and archive cases).
- `python3 tools/net/netlog.py selftest`: passed. `python3 -I tools/mods/modhash.py --selftest`: 0 failed.
- `tools/mods/mods_test.sh -d 91`: **PASS** in 289 s, every check:

  ```
  PASS  1 stock          PASS  5 enemy          PASS  9 bad
  PASS  2 stock-empty    PASS  6 pellet         PASS  10 record
  PASS  3 identity       PASS  7 archive        PASS  11 refuse
  PASS  4 arena          PASS  8 conflict       PASS  12 launcher
  ```

  Check 1 now also runs `--no-mods` over the arena folder (golden, no mods line). Check 4's
  picture rule was flaky at the first verification: with the onion's beam in different phases in
  the two stock pictures the noise was 742, the old limit (3 × noise) 2226, and the arena picture
  differs by 1105 to 1854 pixels, so it failed 3 runs in 4. The rule now paints out the pixels
  where the two stock pictures differ and compares with both (section 11 table). `-c arena` three
  more times passed in all three phase cases: 1105 pixels with noise 742, 1105 with noise 0, and
  1854 with noise 0 (the arena picture in the other phase); a picture of a mod that changes nothing
  scores 0 or about 740.

  Arena first differs from golden at tick 30 (`n=` 2/20/34/19 against 2/20/22/19). The identity
  run's `mppm: mods: hash` line equals `modhash.py` for the same folder and build id. Conflict:
  the new caveinfo gives 16 enemies against arena's 22; the reverse order is identical to arena.
  Pellet evidence: **checksum** (`OUT/pellet_evidence.txt`); the pellet run first differs from
  golden at tick 210 in `rng`, so `make_test_mods.py` now defaults to `--pellet-evidence checksum`.
  The bad mods give these errors (one `skipped` line each, arena still mounted): `mod.json is not
  valid JSON.`, `The path contains a . or .. part.`, `The path must be relative to the mod folder
  (no leading /).`, `The file or a folder above it is a symbolic link; a mod cannot contain links.`,
  `"id" must equal the folder name.`, `The file is missing.`, `Needs Moonsprout 99.0 or newer.`,
  `The VS stage list has 11 stages; the stage select shows at most 10.`, `The file is larger than
  64 MiB.`, `mod.json nests deeper than 16 levels.`
- `tools/net/desync_ci.sh --quick -d 91`: **PASS** in 396 s, golden L10/Ls/Le identical.
- The stub grep of the done test prints nothing. `git diff --stat online/m5.2 -- src include`:
  empty (no upstream edit, so the MWCC matching build is unaffected).
- Mods window, keyboard only, over a folder with a listed mod whose folder is gone: every list
  entry shows its state word (names give way to the ellipsis); a bare long id as the heading stays
  on one line; Move Up on the mod below the missing one swaps with the mod above it; "Position i of
  n" counts the present mods; the version label shows " · 1 mod" in the standalone flow.
- Smoke (at integration): a plain boot with no mods folder reaches the title with no `mppm: mods:`
  line and creates no folder; the Mods window and Settings open from the live start menu; a
  hand-made loose mod of `user/Matoba/challenge/stages.txt` (25 red + 25 blue on the first
  Challenge stage) shows in the Challenge menu and in the cave.

Fix round 2 (second verification): a loose file replacing a disc file with upper-case letters
renamed the FST entry to lower case, and a mod replacing `/AudioRes/PSound.aaf` made the boot
assert (`system.cpp:1173`, via the case-sensitive `JKRFileCache::findFile`); overlays are now
registered under the disc's spelling (section 3) and the identity test mod replaces `PSound.aaf`.
Also: a content match on an archive with a real disc entry number of another copy no longer
applies (section 13); a path through a file in an archive is reported as a nested archive or a
file-as-folder; the Mods window shows names on their own line, can remove a missing mod from the
list, shows the running game's count and content hash in game, hides its prompts after a mouse
click (shared `<button-prompts>` markup), and reads at most 512 MiB of the mods that are off.
After the round: `mods_test.sh -d 91` **PASS** in 279 s (all 12 checks; identity boots with
`PSound.aaf` replaced, golden C lines; arena picture 1105 pixels, noise 0, limit 900); again
`-c identity,enemy,bad` PASS after the last edit; `p2mods_selftest` 19 passed; `modhash.py
--selftest` and `netlog.py selftest` pass; `desync_ci.sh --quick -d 91` **PASS** in 374 s; a
hand-made `archives/enemy/parm/enemyParms.szs/kochappy/enemyparm.txt/x.txt` gives "The path
kochappy/enemyparm.txt in the archive /enemy/parm/enemyparms.szs is a file, not a folder."; the
Mods window screenshots show the two-line entries, **Remove From List** dropping a missing line
from `load_order.txt`, and the prompt row gone after a click and back after a key.

Fix round 3 (minor items of the final review): the Mods window's detail pane scrolls back to the
top when another item is selected; with mods on, the mount logs a warning for a folder whose name
is not a mod id and for a `-<id>` line whose folder is missing (the folder one also shows under the
mod's Problems); the in-game window computes the running content hash on the scan's worker ("…"
until then) instead of on the UI thread; `mods_restart_pending()` waits only for a scan that follows
a load-order change, and the scan runs on a detached thread so Quit never waits for it; the restart
rule compares the folder's mods hash with `mounted().folderHash` (the folder at the boot), so a
disc-dependent drop is still no change but any edit, update, reorder or toggle is; load order now
decides between a whole-archive replacement and inner files (section 3, README 5), listed in the
conflicts with the whole-archive note; section 6 documents the cost of the eager Dvd/Aram/Comp
copies; section 12 lists every deviation. New selftest cases: `whole_archive_vs_inner`,
`off_notices_folder_and_gone`, `restart_rule_folder_hash`. New done-test check 13 (`whole`) and
two more findings in check 9. After the round (2026-11-23, display 89): `p2mods_selftest` 22
passed, 0 failed; `modhash.py --selftest` 0 failed; `mods_test.sh -d 89` **PASS** in 310 s, all 13
checks (13: `archive-whole` golden with three conflict lines and no inner override applied,
`archive-whole-rev` three reverse lines, `applied (mem`, C lines identical to check 5's);
`desync_ci.sh --quick -d 89` **PASS** in 383 s; no `src/` or `include/` change. Mods window
screenshots (keyboard, headless): a tall detail pane scrolled to its bottom, then the next mod
opening at its top; the conflict lines ending in "(whole archive)"; the `Bad Folder Name` warning
under Problems.

Two done-test fixes were made at integration: the newfiles caveinfo also lowers two spawn
weights (`{f002}` alone has no effect within 3600 ticks), and the launcher check presses Escape
twice (the first returns the focus from the detail pane to the list).

Results before integration (tests package, stock `online/m5.2` build, which has no mods code):

- `python3 -I tools/mods/modhash.py --selftest`: all checks pass (`len(S) = 283`, the five
  hashes of section 5, path rules).
- `make_test_mods.py` builds all 10 scenarios from the USA disc with the built-in reader and
  with `dtk` (identical files).
- `modhash.py` reports every good mod valid and each of the 10 bad mods with its own error.
- `mods_test.sh -c stock`: PASS. `-c stock-empty`: the checksum part passes; the `none on` line
  is missing because the stock build ignores `MPPM_MODS_DIR` (expected).
- `desync_ci.sh --quick`: PASS (no-mod behaviour unchanged).

## 12. Deviations from the plan

Every place where the build departs from plan 7a (and the replay line of 7b). The D numbers are
those of the M5.2 design; the others have none.

- **D1: module location.** The plan put the overlay in `port/sdk/dvd_fixes.cpp` or a neighbouring
  `port/sdk/dvd_overlay.cpp`, and the archive registry and disc-side hashing in
  `port/sdk/dvd_overlay.cpp`. It is `port/mods/` instead (archive part in
  `port/mods/archive_mount.cpp` and `port/mods/pure/archive_index.cpp`), and `dvd_fixes.cpp` is
  unchanged. Same effect: Aurora's overlay sits below the `p2_` wrappers, so they reach it.
- **D6: overlay data source.** The plan registers callbacks "backed by `SDL_IOFromFile`". Every
  winning file is instead read into a port-owned blob at the mount (it is read anyway to hash it)
  and the callbacks serve from memory: they are thread-safe by construction, and the bytes served
  are exactly the bytes hashed (a file changed on disk after the hash cannot split peers). Cost:
  the winning files stay in memory for the process (at most 512 MiB with the limits).
- **Mount once.** The plan notes that Aurora's file set "can be replaced at runtime" so the lobby
  can swap mod sets. M5.2 mounts once per process: the blobs are never freed and the archive
  registry is immutable after `install`; a different set needs a restart (D16). Swapping at a
  section boundary is left to M5.3b.
- **D5: disc-dependent failures.** The plan names no rule for problems found only with the disc.
  A mod that fails one at the mount (archive not on the disc, inner file missing, loose path that
  is a disc folder, file changed while starting) is dropped whole and the winners are recomputed
  (a round loop), so the mounted set can be smaller than the folder's.
- **Path normalization.** Plan step 2 normalizes each path with the `sdkPath` rule (`a/b//c/d` →
  `/c/d`). Manifest paths must instead already be in normal form: an empty part, `.`/`..`, a
  leading `/`, `\`, `:` or a Windows device name is an error, not rewritten (section 4), so a path
  always means what it says. Keys are `/` + the lower-case path, as in the plan.
- **Hash framing.** The plan's content hash concatenates the fields with `‖`. The serialization
  `S` (section 5) frames them: `u32le` length before each string, a `u32le` file count per mod and
  a `u64le` size, so two different sets cannot give the same bytes. `build_hash` is the build id
  `p2_net_build_id()` (FNV-1a 64 of the executable) as `u64le`. A second value, the mods hash
  (`"mppm-mods-v1" ‖ S`, no build), is added for the replay gate (D7).
- **D7: what the replay gate compares.** The plan says a replay refuses different content. The
  gate compares the **mods hash**, not the content hash, because the content hash includes the
  build and the determinism corpus is replayed across builds on purpose. A different build stays a
  warning. The content hash is stored in the header and shown; M5.3b's handshake compares it in
  full.
- **D8: synthetic replays.** Not content-checked (header flags bit 0), so the same synthetic log
  can be replayed with and without mods. A notice line says so when mods are on.
- **D12: re-mounts of a converted buffer.** The plan looks the set up in a port-side
  `mDataInfo → override set` map. The set id is written into the info block's padding byte
  `_1B[3]` instead, so no map can go stale when a heap block is reused.
- **D13: override memory, Dvd/Aram/Comp.** As the plan says, these archives get their override
  from the archive's heap; the copy is made at the mount for every overridden file (not lazily),
  and for Comp flag 0x10 is cleared so its destructor frees it. Section 6 gives the memory cost and
  why it is not lazy.
- **D14: deferred archive features.** Adding files to an archive (v2), nested archives ("the same
  scheme covers nested archives") and the repack fallback are not built. An inner path that the
  archive does not hold fails with "Adding files to an archive is not supported yet."; a path
  through a file that is itself an archive (`archives/outer.szs/inner.arc/file`) fails with "The
  file inner.arc in the archive /outer.szs is itself an archive. Archives inside archives are not
  supported yet; replace the whole /outer.szs as a loose file instead." No M5.2 content needs
  either (the done row asks only for replaced files in `enemyParms.szs` and `pelletlist_us.szs`),
  and replacing the whole archive as a loose file covers both for authors until v2.
- **Yaz0 decoder.** Plan 7a ("Finding the archive") says archives are Yaz0-decoded with the
  existing `JKRDecompress`. The overlay uses its own bounded decoder (`pure/archive_index.cpp`,
  `yaz0_decode`; section 6): it decodes into a host buffer outside the game's JKR heaps, and
  must fail cleanly on a truncated stream or an oversized output, which `JKRDecompress` does not
  check. It is a pure function, so `p2mods_selftest` covers it.
- **Whole archive against inner files.** The plan's rule is "last wins" per normalized path, and a
  whole-archive replacement (`/enemy/parm/enemyparms.szs`) and an inner file
  (`/enemy/parm/enemyparms.szs:kochappy/enemyparm.txt`) have different paths. The build applies
  the load order between them too: the mod that replaces the archive counts as giving each inner
  file at its place, so an inner file from a mod before it is not used, one from a mod after it
  goes on top, and the pair is a conflict either way (section 3, README 5; fix round 3 — before it,
  inner files always went on top).
- **D15: validators.** The plan checks "at most 10 entries" for the VS list and "at least 30" for
  Challenge. The build counts the top-level `{}` blocks and requires the first number to equal
  them (VS 1..10, Challenge and `kfes-stages.txt` at least 30), and adds the loose-archive magic
  check and the text warnings (section 4).
- **Manifest and folder rules beyond the plan.** An id is 1..64 characters and must equal its
  folder's name (the plan says `[a-z0-9-]+`, unique); a folder whose name is not an id is listed
  with a warning and never loads. Size limits (64 MiB per file, 16 MiB per inner file, 256 MiB per
  mod, 512 MiB in all), at most 256 mod folders and a 64 KiB, 1024-line `load_order.txt` are added.
- **D16: restart rule.** In the start menu over a booted game, a mod change does not relaunch at
  once; it makes **Play** restart Moonsprout first (like the Save Manager). A relaunch per toggle
  would restart once per click while several mods are changed. *Open question for the user:* is a
  seamless relaunch per change wanted instead?
- **D17: where the content hash is shown, and when it is computed.** Plan 7a says the hash "is
  computed once at mount time and shown on the launcher's about/status line". It is shown in the
  Mods window's Overview (8 digits in the header, all 64 below); the start menu's version label
  shows only " · N mods". With mods on it is computed at the mount (the `mppm: mods: hash` line);
  with none on it is computed lazily (for a recording, the harness or the Mods window, on a worker
  thread, in game too), because it costs about 0.2 s of FNV over the executable and a plain boot
  should not pay for it (section 5).

## 13. Known limits

- Live-menu mod changes restart on **Play**, not at once (D16).
- The launcher does not keep comments in `load_order.txt` when it rewrites it.
- The pellet override's effect in a 3600-tick Battle replay is not guaranteed to reach the
  checksum; check 6 falls back to the `pellet-bmd` model swap and prints which evidence it used.
- The content hash costs about 0.2 s of FNV over the executable the first time; only with mods, a
  recording or the Mods window open (worker thread).
- Shared `EMM_Mem` override buffers rely on the port's in-place converters being idempotent (see
  section 6). Two threads mounting the same modded archive at the same moment would race on a
  converter's mark check (never seen in a run; the game mounts these archives from one thread).
- Identical archive tables between two targeted archives make buffer mounts ambiguous; logged at
  install. None among the stock targets. The disc holds about 311 groups of byte-identical archives
  (for example `/user/Abe/Pellet/{jpn,pal,us}/`): a path mount (DVD, ARAM, Comp, Mem by entry)
  matches only the set of its own entry number, so overriding one copy leaves the others stock;
  only a buffer mount, whose `mEntryNum` is not a disc entry, takes a content match alone.
- The validators check stage counts, sizes and basic text sanity only. Bounds-checked parsers for
  the game's own formats are a 7b follow-up: a malformed caveinfo can still assert in the game.
- `EMM_Dvd`, `EMM_Aram` and `EMM_Comp` overrides are implemented but no test data targets them
  (only JAudio archives use those modes); only the archive self-test's pure parts exercise them.
- Screenshots at tick 60 of a paced replay are not pixel-identical between runs: the onion's beam
  and lights are not tick-locked and come in one of two phases (about 740 pixels apart at fuzz
  15%). The picture checks leave out the pixels where the two stock pictures differ and compare
  with both; the limit (900) sits above one phase difference, which still counts when both stock
  pictures share a phase and the mod's picture has the other. The arena mod's pellet posies give
  about 1100 pixels (1850 with the onion).
