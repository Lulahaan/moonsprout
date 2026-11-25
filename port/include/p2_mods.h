/* Data-mod overlay, milestone M5.2 (docs/ONLINE_MULTIPLAYER.md 7a and 7b, docs/mods/README.md,
 * docs/online/M5_2_MODS.md).
 *
 * The C interface for host code: port/main.cpp (init and mount), port/assets/rarc.cpp (the
 * in-archive splice) and port/net/net.cpp (the content hash in the record header). The launcher
 * uses the C++ view in p2_mods.hpp. Host-only: game code (src/, include/) never includes this.
 *
 * With mods off (--no-mods), no mods folder, no load_order.txt or no enabled valid mod, every
 * function below is a no-op or returns 0 / "" / zero bytes, nothing is registered with Aurora,
 * p2HostTables is untouched and nothing is logged: offline behaviour is unchanged.
 *
 * Flags (the flag wins over the environment):
 *   --mods-dir DIR | --mods-dir=DIR   MPPM_MODS_DIR=DIR   use DIR as the mods folder
 *   --no-mods                         MPPM_NO_MODS=1      no mods this run (the Mods window says so)
 *
 * Ownership: "core" = port/mods/ except the files named below, "archive" = port/mods/archive_mount.cpp,
 * port/mods/pure/archive_index.*, port/assets/rarc.cpp. Committed in step 0; only core edits it.
 */
#ifndef P2_MODS_H
#define P2_MODS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- core: port/mods/mods.cpp ------------------------------------------------------------- */

/* main(), right after p2_net_init: reads the flags and the environment. No file access. */
void p2_mods_init(int argc, char** argv);

/* The mods folder: --mods-dir, else MPPM_MODS_DIR, else SDL_GetPrefPath(NULL, "mppm") + "mods"
 * (the launcher's data folder, branding::kDataDirName). "" when mods are off for this run
 * (--no-mods, or no data folder). Resolved on first call; the pointer stays valid. */
const char* p2_mods_dir(void);

/* main(), once aurora_dvd_open has succeeded and before the launcher's show_prelaunch: scans the
 * folder, validates, registers the winning loose files with Aurora's overlay
 * (aurora_dvd_overlay_callbacks / aurora_dvd_overlay_files), checks and installs the archive
 * overrides (mppm::mods::archive), computes the mods hash and always reports it to the seam with
 * p2_net_set_content (stock: zero hash, ""). Returns the number of mods mounted. Runs once; later
 * calls return the same number. */
int p2_mods_mount(void);

/* Mods mounted (0 before p2_mods_mount and for stock content). */
int p2_mods_count(void);

/* SHA-256 of the mounted mod set ("mppm-mods-v1" || set bytes, docs/online/M5_2_MODS.md);
 * 32 zero bytes when p2_mods_count() == 0. */
void p2_mods_hash(uint8_t out[32]);

/* The content hash of plan 7a: SHA-256("mppm-content-v1" || build id (u64 LE) || set bytes).
 * Stock content hashes the build only. Computed on the first call (p2_net_build_id reads the
 * executable once); thread-safe. */
void p2_mods_content_hash(uint8_t out[32]);

/* The mounted mods as "id@version,id@version" in load order (lowest priority first); "" for
 * stock. The pointer stays valid. */
const char* p2_mods_list(void);

/* ---- archive: port/mods/archive_mount.cpp (registry) for port/assets/rarc.cpp -------------- */

/* One replaced file inside one archive. `data` is port-owned, 32-byte aligned, lives for the
 * process, and has at least size + 32 readable bytes; the bytes after `size` are zero (the
 * game's text parsers read with RamStream(data, -1) and stop at a NUL). */
typedef struct P2ModsArcFile {
	uint32_t index;        /* index into the archive's file table (SDIFileEntry), never a directory */
	uint32_t size;         /* bytes */
	const uint8_t* data;
	const char* innerPath; /* normalized, e.g. "kochappy/enemyparm.txt" (for logs) */
} P2ModsArcFile;

/* All overrides of one archive. */
typedef struct P2ModsArcSet {
	uint32_t id;             /* 1..254; stored in the converted info block (see rarc.cpp) */
	const char* archivePath; /* normalized disc path, e.g. "/enemy/parm/enemyparms.szs" */
	int32_t entryNum;        /* the archive file's DVD entry number (after the loose overlay), -1 if none */
	uint32_t count;
	const P2ModsArcFile* files;
} P2ModsArcSet;

/* 1 once at least one archive override is installed. rarc.cpp does nothing new while it is 0. */
int p2_mods_arc_any(void);

/* p2HostTables, before the info block is converted: `rawInfo` points at the SArcDataInfo still in
 * disc byte order. Matches it by content (FNV-1a 64 of the info block up to the end of its string
 * table, docs/online/M5_2_MODS.md) against the installed sets; `entryNum` (JKRArchive::mEntryNum)
 * breaks ties between sets with identical tables. NULL when no set matches. Thread-safe (the
 * registry is immutable after install). */
const P2ModsArcSet* p2_mods_arc_find(const void* rawInfo, int32_t entryNum);

/* The set with this id (1..254), NULL otherwise: re-mounts of an already converted buffer. */
const P2ModsArcSet* p2_mods_arc_get(uint32_t id);

/* rarc.cpp reports each file it spliced (ok = 1) or could not (ok = 0, e.g. no heap memory).
 * mountMode is JKRArchive::EMountMode (1 Mem, 2 Aram, 3 Dvd, 4 Comp). Logs the first report of
 * each (set, file) only:
 *   mppm: mods: archive override <archivePath>:<innerPath> applied (<mem|aram|dvd|comp>, <size> bytes)
 *   mppm: mods: archive override <archivePath>:<innerPath> failed (<mode>): out of memory          */
void p2_mods_arc_note(const P2ModsArcSet* set, uint32_t fileIndexInSet, int mountMode, int ok);

#ifdef __cplusplus
}
#endif

#endif /* P2_MODS_H */
