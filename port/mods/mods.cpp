// Data-mod overlay, milestone M5.2 (docs/ONLINE_MULTIPLAYER.md 7a and 7b): flags, the mount
// sequence (design section 3) and the C++ view for the launcher (p2_mods.hpp). The pure parts
// (manifest, load order, paths, validators, set serialization) are in port/mods/pure/.
//
// With mods off, no mods folder, no load_order.txt or nothing enabled, nothing is registered with
// Aurora and nothing is logged (unless the folder came from --mods-dir / MPPM_MODS_DIR: one
// "none on" line), and p2_net_set_content gets the stock content.
#include "p2_mods.h"
#include "p2_mods.hpp"
#include "p2_net.h"

#include "../launcher/branding.hpp"
#include "archive.hpp"
#include "overlay.hpp"
#include "pure/modset.hpp"
#include "pure/load_order.hpp"
#include "pure/paths.hpp"
#include "pure/sha256.hpp"

#include <aurora/dvd.h>
#include <dolphin/dvd.h>
#include <mppm_version.h> // generated at configure time (CMakeLists.txt)
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

using mppm::mods::Hash;
using mppm::mods::Issue;
using mppm::mods::ModInfo;
using mppm::mods::Severity;
namespace pure = mppm::mods::pure;

struct Flags {
	bool off      = false;   // --no-mods / MPPM_NO_MODS=1
	bool explicit_ = false;  // the folder came from --mods-dir / MPPM_MODS_DIR
	std::string dir;         // that folder
};
Flags sFlags;

std::once_flag sDirOnce;
std::string sDir;

bool sMountRan = false;
int sCount     = 0;
Hash sModsHash{};
std::string sList;
mppm::mods::Mounted sMounted;

std::mutex sContentMutex;
bool sContentDone = false;
Hash sContent{};

bool env_on(const char* name)
{
	const char* v = std::getenv(name);
	return v != nullptr && std::strcmp(v, "1") == 0;
}

const char* sev_word(Severity s) { return s == Severity::Error ? "error" : "warning"; }

void log_issue(const std::string& id, const Issue& issue)
{
	std::fprintf(stderr, "mppm: mods: %s %s: %s: %s\n", sev_word(issue.severity), id.c_str(),
	             issue.path.empty() ? "-" : issue.path.c_str(), issue.message.c_str());
}

// Marks a mod failed at the mount (disc-dependent or read problem) and logs the reason.
void fail_mod(ModInfo& mod, std::string path, std::string message)
{
	Issue issue{Severity::Error, std::move(path), std::move(message)};
	log_issue(mod.id, issue);
	mod.issues.push_back(std::move(issue));
	mod.valid = false;
}

void report_stock()
{
	const uint8_t zero[32] = {};
	p2_net_set_content(zero, "");
}

// One winning file of the mounted set.
struct Winner {
	size_t mod;  // index into scan.mods
	size_t file; // index into files[mod]
};

} // namespace

// ---- C interface ---------------------------------------------------------------------------

extern "C" void p2_mods_init(int argc, char** argv)
{
	bool flagOff = false, flagDir = false;
	std::string dir;
	for (int i = 1; i < argc; i++) {
		const char* a = argv[i];
		if (a == nullptr) continue;
		if (std::strcmp(a, "--no-mods") == 0) {
			flagOff = true;
		} else if (std::strcmp(a, "--mods-dir") == 0 && i + 1 < argc && argv[i + 1] != nullptr) {
			flagDir = true;
			dir     = argv[++i];
		} else if (std::strncmp(a, "--mods-dir=", 11) == 0) {
			flagDir = true;
			dir     = a + 11;
		}
	}
	if (flagOff) {
		sFlags.off = true;
	} else if (flagDir) {
		sFlags.explicit_ = true;
		sFlags.dir       = dir;
	} else if (env_on("MPPM_NO_MODS")) {
		sFlags.off = true;
	} else if (const char* env = std::getenv("MPPM_MODS_DIR"); env != nullptr && env[0] != '\0') {
		sFlags.explicit_ = true;
		sFlags.dir       = env;
	}
}

extern "C" const char* p2_mods_dir(void)
{
	std::call_once(sDirOnce, [] {
		if (sFlags.off) return;
		if (sFlags.explicit_) {
			sDir = sFlags.dir;
			return;
		}
		// The launcher's data folder (<userdir>), so the mods folder always sits beside its settings.
		char* pref = SDL_GetPrefPath(nullptr, mppm::branding::kDataDirName);
		if (pref != nullptr) {
			sDir = std::string(pref) + "mods";
			SDL_free(pref);
		}
	});
	return sDir.c_str();
}

extern "C" int p2_mods_mount(void)
{
	if (sMountRan) return sCount;
	sMountRan     = true;
	sMounted.ran  = true;
	const std::string dir = p2_mods_dir();
	if (dir.empty()) {
		sMounted.scan.off = sFlags.off;
		report_stock();
		return 0;
	}
	const auto noneOn = [&](bool always) {
		if (always || sFlags.explicit_) std::fprintf(stderr, "mppm: mods: folder %s: none on\n", dir.c_str());
		report_stock();
		return 0;
	};
	std::error_code ec;
	if (!fs::is_directory(dir, ec) || !fs::exists(fs::symlink_status(fs::path(dir) / "load_order.txt", ec))) {
		sMounted.scan.dir    = dir;
		sMounted.scan.exists = fs::is_directory(dir, ec);
		return noneOn(false);
	}

	pure::FullScan full = pure::scan_dir(dir, MPPM_VERSION_STRING, pure::ScanOptions{false, false});
	mppm::mods::Scan& scan = full.scan;
	sMounted.folderHash = scan.modsHash; // before any disc-dependent drop (restart rule, Mods window)
	bool anyEnabled = false;
	for (const ModInfo& m : scan.mods) anyEnabled = anyEnabled || m.enabled;
	if (!anyEnabled) {
		sMounted.scan = std::move(scan);
		return noneOn(false);
	}

	// The scan's findings for this run: folder issues, then each enabled mod's issues.
	for (const Issue& i : scan.issues) log_issue("-", i);
	for (const ModInfo& m : scan.mods) {
		if (!m.enabled) continue;
		for (const Issue& i : m.issues) log_issue(m.id, i);
		if (m.present && !m.valid) std::fprintf(stderr, "mppm: mods: skipped %s (errors above)\n", m.id.c_str());
	}
	// Mods that are not on: only a folder that can never load and a listed folder that is gone.
	for (const auto& [id, issue] : pure::off_notices(scan)) log_issue(id, issue);

	// Blobs per (mod, file), read once; never freed (Aurora and the archive registry keep them).
	std::map<std::pair<size_t, size_t>, mppm::mods::overlay::Blob*> blobs;
	bool registered = false;
	bool done       = false;
	std::vector<Winner> loose, inner;
	// Every round that does not finish fails at least one mod of the set (and a failed mod never
	// comes back), so this ends after at most one round per mod plus the last: the cap is only a
	// guard. Each round checks the current winner of every key; when it fails, the next mod with that
	// key wins the next round.
	const size_t maxRounds = scan.mods.size() + 1;
	for (size_t round = 0; round < maxRounds && !done; round++) {
		std::vector<size_t> set;
		for (size_t i = 0; i < scan.mods.size(); i++) {
			const ModInfo& m = scan.mods[i];
			if (m.enabled && m.present && m.valid) set.push_back(i);
		}
		std::vector<bool> failed(scan.mods.size(), false);

		// Read and re-hash every listed file of the set.
		for (const size_t mi : set) {
			for (size_t fi = 0; fi < full.files[mi].size() && !failed[mi]; fi++) {
				if (blobs.count({mi, fi})) continue;
				const pure::FileRec& f = full.files[mi][fi];
				std::string bytes;
				if (!pure::read_file(f.path, f.archive ? pure::kMaxInnerFileBytes : pure::kMaxFileBytes, &bytes) ||
				    bytes.size() != f.size || pure::Sha256::of(bytes.data(), bytes.size()) != f.sha) {
					fail_mod(scan.mods[mi], f.entry, "The file changed while the game was starting.");
					failed[mi] = true;
					break;
				}
				auto* blob = new mppm::mods::overlay::Blob;
				uint8_t* p = mppm::mods::overlay::alloc_blob(f.size, blob);
				if (p == nullptr) {
					delete blob;
					fail_mod(scan.mods[mi], f.entry, "Out of memory while loading the file.");
					failed[mi] = true;
					break;
				}
				if (f.size != 0) std::memcpy(p, bytes.data(), f.size);
				blobs[{mi, fi}] = blob;
			}
		}
		bool anyFailed = false;
		for (const size_t mi : set) anyFailed = anyFailed || failed[mi];
		if (anyFailed) {
			for (const size_t mi : set) {
				if (failed[mi]) std::fprintf(stderr, "mppm: mods: skipped %s (errors above)\n", scan.mods[mi].id.c_str());
			}
			continue;
		}

		// Winners: last wins per key, and a whole-archive replacement beats the inner files of that
		// archive from mods before it (pure::pick_winners, README 5).
		std::vector<pure::KeyedMod> keyed;
		for (const size_t mi : set) keyed.push_back({scan.mods[mi].id, scan.mods[mi].keys});
		loose.clear();
		inner.clear();
		for (const auto& [key, k] : pure::pick_winners(keyed)) {
			const size_t mi = set[k];
			for (size_t fi = 0; fi < full.files[mi].size(); fi++) {
				if (full.files[mi][fi].key != key) continue;
				(full.files[mi][fi].archive ? inner : loose).push_back({mi, fi});
				break;
			}
		}

		// Loose files to Aurora.
		if (!loose.empty() || registered) {
			// Register each file under the disc's own spelling when the disc has it. Keys are lower
			// case, and Aurora gives a replaced FST entry the overlay's name; JKRFileCache::findFile
			// compares DVDReadDir names case-sensitively, so "/audiores/psound.aaf" would hide
			// /AudioRes/PSound.aaf from JKRGetResource and fail the boot.
			std::vector<std::string> names;
			names.reserve(loose.size());
			for (const Winner& w : loose) {
				const pure::FileRec& f = full.files[w.mod][w.file];
				std::string name       = f.key;
				const s32 entry        = DVDConvertPathToEntrynum(f.key.c_str());
				char discPath[512];
				if (entry >= 0 && DVDConvertEntrynumToPath(entry, discPath, sizeof(discPath))
				    && pure::ascii_lower(discPath) == f.key) {
					name = discPath;
				}
				names.push_back(std::move(name));
			}
			std::vector<AuroraOverlayFile> files;
			for (size_t k = 0; k < loose.size(); k++) {
				const Winner& w        = loose[k];
				const pure::FileRec& f = full.files[w.mod][w.file];
				files.push_back({names[k].c_str(), blobs[{w.mod, w.file}], (size_t)f.size});
			}
			std::vector<s32> entryNums(files.size(), -1);
			if (!files.empty()) mppm::mods::overlay::install_callbacks();
			aurora_dvd_overlay_files(files.empty() ? nullptr : files.data(), files.size(),
			                         files.empty() ? nullptr : entryNums.data());
			registered = true;
			for (size_t k = 0; k < loose.size(); k++) {
				if (entryNums[k] >= 0) continue;
				ModInfo& m = scan.mods[loose[k].mod];
				fail_mod(m, full.files[loose[k].mod][loose[k].file].entry,
				         "The game could not use this path: a folder on the disc has that name.");
				failed[loose[k].mod] = true;
			}
		}

		// Archive overrides.
		std::vector<mppm::mods::archive::Entry> entries;
		for (const Winner& w : inner) {
			const pure::FileRec& f = full.files[w.mod][w.file];
			const auto* blob       = blobs[{w.mod, w.file}];
			entries.push_back({scan.mods[w.mod].id, f.archivePath, f.innerPath, blob->data, (uint32_t)blob->size});
		}
		if (!entries.empty()) {
			for (const auto& p : mppm::mods::archive::check(entries)) {
				if (p.entry >= inner.size()) continue;
				const Winner& w = inner[p.entry];
				fail_mod(scan.mods[w.mod], full.files[w.mod][w.file].entry, p.message);
				failed[w.mod] = true;
			}
		}

		anyFailed = false;
		for (const size_t mi : set) {
			if (!failed[mi]) continue;
			anyFailed = true;
			std::fprintf(stderr, "mppm: mods: skipped %s (errors above)\n", scan.mods[mi].id.c_str());
		}
		if (anyFailed) continue;
		if (!entries.empty()) mppm::mods::archive::install(entries);
		done = true;
	}
	if (!done) {
		// Cannot happen (see the loop); mount nothing rather than a set that was never checked.
		std::fprintf(stderr, "mppm: mods: the mod set did not settle; no mod is used\n");
		for (ModInfo& m : scan.mods) {
			if (m.enabled && m.present && m.valid) fail_mod(m, "", "The mod set did not settle while the game was starting.");
		}
		if (registered) aurora_dvd_overlay_files(nullptr, 0, nullptr);
		loose.clear();
		inner.clear();
	}

	pure::finish_set(full);
	sMounted.scan = scan;
	if (scan.enabledValid == 0) return noneOn(true); // enabled mods failed: say so

	sCount    = scan.enabledValid;
	sModsHash = scan.modsHash;
	std::string names;
	for (const ModInfo& m : scan.mods) {
		if (!(m.enabled && m.present && m.valid)) continue;
		if (!sList.empty()) {
			sList += ',';
			names += ", ";
		}
		sList += m.id + "@" + m.version;
		names += m.id + "@" + m.version;
	}
	std::fprintf(stderr, "mppm: mods: folder %s: %d on (%s), %zu files, %zu archive files, %zu conflicts\n", dir.c_str(),
	             sCount, names.c_str(), loose.size(), inner.size(), scan.conflicts.size());
	for (const mppm::mods::Conflict& c : scan.conflicts) {
		std::string losers;
		for (const std::string& l : c.losers) losers += (losers.empty() ? "" : ", ") + l;
		const std::string whole = c.archiveMod.empty() ? "" : " (" + c.archiveMod + " replaces the whole archive)";
		std::fprintf(stderr, "mppm: mods: conflict %s: %s overrides %s%s\n", c.key.c_str(), c.winner.c_str(),
		             losers.c_str(), whole.c_str());
	}
	uint8_t content[32];
	p2_mods_content_hash(content);
	Hash contentHash;
	std::memcpy(contentHash.data(), content, 32);
	std::fprintf(stderr, "mppm: mods: hash %s content %s\n", pure::hex_of(sModsHash).c_str(),
	             pure::hex_of(contentHash).c_str());
	p2_net_set_content(sModsHash.data(), sList.c_str());
	return sCount;
}

extern "C" int p2_mods_count(void) { return sCount; }

extern "C" void p2_mods_hash(uint8_t out[32]) { std::memcpy(out, sModsHash.data(), 32); }

extern "C" void p2_mods_content_hash(uint8_t out[32])
{
	std::lock_guard<std::mutex> lock(sContentMutex);
	if (!sContentDone) {
		const std::vector<uint8_t> none;
		const Hash h = pure::content_hash_of(p2_net_build_id(), sCount > 0 ? sMounted.scan.setBytes : none);
		if (!sMountRan) {
			std::memcpy(out, h.data(), 32); // before the mount: stock, not cached
			return;
		}
		sContent     = h;
		sContentDone = true;
	}
	std::memcpy(out, sContent.data(), 32);
}

extern "C" const char* p2_mods_list(void) { return sList.c_str(); }

// ---- C++ view (p2_mods.hpp) ----------------------------------------------------------------

namespace mppm::mods {

ScanArgs scan_args()
{
	return ScanArgs{fs::path(p2_mods_dir()), sFlags.off};
}

Scan scan(const ScanArgs& args)
{
	return pure::scan_dir(args.dir, MPPM_VERSION_STRING, pure::ScanOptions{args.off, true}).scan;
}

Scan scan() { return scan(scan_args()); }

const Mounted& mounted() { return sMounted; }

bool ensure_dir(std::string* error)
{
	const std::string dir = p2_mods_dir();
	if (sFlags.off) {
		if (error) *error = "Mods are off for this run.";
		return false;
	}
	if (dir.empty()) {
		if (error) *error = "There is no data folder for mods.";
		return false;
	}
	std::error_code ec;
	fs::create_directories(dir, ec);
	if (ec || !fs::is_directory(dir, ec)) {
		if (error) *error = "Could not create the mods folder.";
		return false;
	}
	return true;
}

bool write_load_order(const std::vector<OrderEntry>& order, std::string* error)
{
	if (!ensure_dir(error)) return false;
	std::vector<OrderEntry> clean;
	for (const OrderEntry& e : order) {
		if (pure::valid_id(e.id)) clean.push_back(e); // never write a newline or other junk
	}
	const std::string text = pure::format_load_order(clean);
	const fs::path dir     = p2_mods_dir();
	const fs::path final_  = dir / "load_order.txt";
#if defined(_WIN32)
	const long pid = static_cast<long>(_getpid());
#else
	const long pid = static_cast<long>(getpid());
#endif
	const fs::path tmp     = dir / (".load_order.txt." + std::to_string(pid) + ".tmp");
	FILE* f = pure::open_file(tmp, "wb");
	if (f == nullptr) {
		if (error) *error = std::string("Could not write load_order.txt: ") + std::strerror(errno) + ".";
		return false;
	}
	bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
	ok      = std::fflush(f) == 0 && ok;
#if defined(_WIN32)
	ok      = _commit(_fileno(f)) == 0 && ok;
#else
	ok      = fsync(fileno(f)) == 0 && ok;
#endif
	ok      = std::fclose(f) == 0 && ok;
	std::error_code ec;
	if (ok) fs::rename(tmp, final_, ec);
	if (!ok || ec) {
		fs::remove(tmp, ec);
		if (error) *error = "Could not write load_order.txt.";
		return false;
	}
	return true;
}

Hash content_hash(const Scan& scan)
{
	return pure::content_hash_of(p2_net_build_id(), scan.enabledValid > 0 ? scan.setBytes : std::vector<uint8_t>{});
}

std::string hex(const Hash& hash, size_t digits) { return pure::hex_of(hash, digits); }

std::string locked_reason()
{
	if (p2_net_active()) return "Locked while a replay or harness session runs.";
	if (sFlags.off) return "Mods are off for this run.";
	return "";
}

} // namespace mppm::mods
