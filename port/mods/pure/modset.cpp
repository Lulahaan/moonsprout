// Data-mod overlay (M5.2): see modset.hpp.
#include "modset.hpp"

#include "load_order.hpp"
#include "manifest.hpp"
#include "paths.hpp"
#include "sha256.hpp"
#include "validate.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <system_error>

namespace mppm::mods::pure {

namespace fs = std::filesystem;

namespace {

void add(std::vector<Issue>& issues, Severity sev, std::string path, std::string message)
{
	issues.push_back({sev, std::move(path), std::move(message)});
}

bool has_error(const std::vector<Issue>& issues)
{
	for (const Issue& i : issues) {
		if (i.severity == Severity::Error) return true;
	}
	return false;
}

// True when `inner` is `outer` or below it (both canonical).
bool inside(const fs::path& outer, const fs::path& inner)
{
	auto o = outer.begin(), i = inner.begin();
	for (; o != outer.end(); ++o, ++i) {
		if (i == inner.end() || *o != *i) return false;
	}
	return true;
}

void put_u32(std::vector<uint8_t>& b, uint32_t v)
{
	for (int i = 0; i < 4; i++) b.push_back((uint8_t)(v >> (8 * i)));
}

void put_u64(std::vector<uint8_t>& b, uint64_t v)
{
	for (int i = 0; i < 8; i++) b.push_back((uint8_t)(v >> (8 * i)));
}

void put_str(std::vector<uint8_t>& b, const std::string& s)
{
	put_u32(b, (uint32_t)s.size());
	b.insert(b.end(), s.begin(), s.end());
}

// Checks one listed entry on disk: every component below the mod folder exists and is not a
// symbolic link, the last is a regular file, and its canonical path is inside the mod folder.
void check_on_disk(const fs::path& modDir, const fs::path& canonMod, std::string_view entry, FileRec& rec,
                   std::vector<Issue>& issues)
{
	std::error_code ec;
	fs::path p = modDir;
	size_t start = 0;
	while (true) {
		const size_t slash = entry.find('/', start);
		const bool last    = slash == std::string_view::npos;
		p /= std::string(entry.substr(start, last ? std::string_view::npos : slash - start));
		const fs::file_status st = fs::symlink_status(p, ec);
		if (ec || st.type() == fs::file_type::not_found) {
			add(issues, Severity::Error, rec.entry, "The file is missing.");
			return;
		}
		if (st.type() == fs::file_type::symlink) {
			add(issues, Severity::Error, rec.entry,
			    "The file or a folder above it is a symbolic link; a mod cannot contain links.");
			return;
		}
		if (last) {
			if (st.type() != fs::file_type::regular) {
				add(issues, Severity::Error, rec.entry, "The path is not a regular file.");
				return;
			}
			break;
		}
		if (st.type() != fs::file_type::directory) {
			add(issues, Severity::Error, rec.entry, "The file is missing.");
			return;
		}
		start = slash + 1;
	}
	const fs::path canon = fs::canonical(p, ec);
	if (ec || !inside(canonMod, canon)) {
		add(issues, Severity::Error, rec.entry, "The file is outside the mod folder.");
		return;
	}
	const uintmax_t size = fs::file_size(p, ec);
	if (ec) {
		add(issues, Severity::Error, rec.entry, "The file could not be read.");
		return;
	}
	rec.size = size;
	rec.path = p;
	if (!rec.archive && size > kMaxFileBytes) {
		add(issues, Severity::Error, rec.entry, "The file is larger than 64 MiB.");
		return;
	}
	if (rec.archive && size > kMaxInnerFileBytes) {
		add(issues, Severity::Error, rec.entry, "A file inside an archive can be at most 16 MiB.");
		return;
	}
	rec.ok = true;
}

// Fills one present mod: manifest, entries, files on disk, sizes, and (readContent) hashes and
// validators. `offBudget` (mods that are not on, launcher view): the bytes still allowed to be read
// for them; a mod larger than what is left is checked by name and size only, so a folder full of
// mods that are off costs at most 512 MiB of reading, like the mods that are on.
void check_mod(ModInfo& mod, std::vector<FileRec>& files, std::string_view buildVersion, bool readContent,
               uint64_t* offBudget)
{
	std::error_code ec;
	const fs::path manifestPath = mod.dir / "mod.json";
	const fs::file_status st    = fs::symlink_status(manifestPath, ec);
	if (ec || st.type() == fs::file_type::not_found) {
		add(mod.issues, Severity::Error, "", "The folder has no mod.json.");
		return;
	}
	if (st.type() == fs::file_type::symlink) {
		add(mod.issues, Severity::Error, "", "mod.json is a symbolic link; a mod cannot contain links.");
		return;
	}
	if (st.type() != fs::file_type::regular) {
		add(mod.issues, Severity::Error, "", "mod.json is not a regular file.");
		return;
	}
	std::string text;
	const uintmax_t jsonSize = fs::file_size(manifestPath, ec);
	if (!ec && jsonSize > kManifestMaxBytes) {
		add(mod.issues, Severity::Error, "", "mod.json is larger than 64 KiB.");
		return;
	}
	if (ec || !read_file(manifestPath, kManifestMaxBytes, &text)) {
		add(mod.issues, Severity::Error, "", "mod.json could not be read.");
		return;
	}
	Manifest m = parse_manifest(text, mod.id, buildVersion);
	mod.name        = m.name.empty() ? mod.id : m.name;
	mod.version     = m.version;
	mod.author      = m.author;
	mod.description = m.description;
	mod.minMppm     = m.minMppm;
	mod.cosmetic    = m.cosmetic;
	mod.issues.insert(mod.issues.end(), m.issues.begin(), m.issues.end());
	mod.fileCount = (uint32_t)m.files.size();

	const fs::path canonMod = fs::canonical(mod.dir, ec);
	if (ec) {
		add(mod.issues, Severity::Error, "", "The mod folder could not be read.");
		return;
	}
	std::map<std::string, size_t> seen;
	for (const std::string& raw : m.files) {
		FileRec rec;
		rec.entry = clip_text(raw, 240);
		EntryKey k = entry_key(raw);
		if (!k.ok) {
			add(mod.issues, Severity::Error, rec.entry, k.error);
			files.push_back(std::move(rec));
			continue;
		}
		rec.key         = k.key;
		rec.archive     = k.archive;
		rec.archivePath = k.archivePath;
		rec.innerPath   = k.innerPath;
		if (!seen.emplace(rec.key, files.size()).second) {
			add(mod.issues, Severity::Error, rec.entry, "Another entry of this mod names the same file (letter case is ignored).");
			files.push_back(std::move(rec));
			continue;
		}
		check_on_disk(mod.dir, canonMod, raw, rec, mod.issues);
		mod.totalBytes += rec.size;
		files.push_back(std::move(rec));
	}
	for (const FileRec& f : files) {
		if (!f.key.empty()) mod.keys.push_back(f.key);
	}
	std::sort(mod.keys.begin(), mod.keys.end());
	mod.keys.erase(std::unique(mod.keys.begin(), mod.keys.end()), mod.keys.end());

	if (mod.totalBytes > kMaxModBytes) {
		add(mod.issues, Severity::Error, "", "The mod is larger than 256 MiB.");
		return;
	}
	if (!readContent) return;
	if (offBudget != nullptr) {
		if (mod.totalBytes > *offBudget) return;
		*offBudget -= mod.totalBytes;
	}
	std::string data;
	for (FileRec& f : files) {
		if (!f.ok) continue;
		if (!read_file(f.path, f.archive ? kMaxInnerFileBytes : kMaxFileBytes, &data) || data.size() != f.size) {
			add(mod.issues, Severity::Error, f.entry, "The file could not be read.");
			f.ok = false;
			continue;
		}
		f.sha    = Sha256::of(data.data(), data.size());
		f.hashed = true;
		validate_file(f.key, f.entry, reinterpret_cast<const uint8_t*>(data.data()), data.size(), mod.issues);
	}
}

} // namespace

std::FILE* open_file(const fs::path& path, const char* mode)
{
#if defined(_WIN32)
	std::wstring wmode;
	for (const char* m = mode; *m != '\0'; m++) wmode += static_cast<wchar_t>(*m);
	return _wfopen(path.c_str(), wmode.c_str());
#else
	return std::fopen(path.c_str(), mode);
#endif
}

bool read_file(const fs::path& path, uint64_t maxBytes, std::string* out)
{
	out->clear();
	FILE* f = open_file(path, "rb");
	if (f == nullptr) return false;
	char buf[65536];
	bool ok = true;
	while (true) {
		const size_t n = std::fread(buf, 1, sizeof(buf), f);
		if (n == 0) {
			ok = !std::ferror(f);
			break;
		}
		if (out->size() + n > maxBytes) {
			ok = false;
			break;
		}
		out->append(buf, n);
	}
	std::fclose(f);
	if (!ok) out->clear();
	return ok;
}

std::vector<uint8_t> serialize_set(const std::vector<SetMod>& mods)
{
	std::vector<uint8_t> s;
	for (const SetMod& m : mods) {
		put_str(s, m.id);
		put_str(s, m.version);
		put_u32(s, (uint32_t)m.files.size());
		std::vector<const SetFile*> sorted;
		for (const SetFile& f : m.files) sorted.push_back(&f);
		std::sort(sorted.begin(), sorted.end(), [](const SetFile* a, const SetFile* b) { return a->key < b->key; });
		for (const SetFile* f : sorted) {
			put_str(s, f->key);
			put_u64(s, f->size);
			s.insert(s.end(), f->sha.begin(), f->sha.end());
		}
	}
	return s;
}

Hash mods_hash_of(const std::vector<uint8_t>& setBytes, int count)
{
	if (count == 0) return Hash{};
	Sha256 h;
	h.update("mppm-mods-v1");
	h.update(setBytes.data(), setBytes.size());
	return h.finish();
}

Hash content_hash_of(uint64_t buildId, const std::vector<uint8_t>& setBytes)
{
	Sha256 h;
	h.update("mppm-content-v1");
	uint8_t le[8];
	for (int i = 0; i < 8; i++) le[i] = (uint8_t)(buildId >> (8 * i));
	h.update(le, 8);
	h.update(setBytes.data(), setBytes.size());
	return h.finish();
}

std::string archive_of_key(std::string_view key)
{
	const size_t colon = key.find(':');
	return colon == std::string_view::npos ? std::string() : std::string(key.substr(0, colon));
}

std::map<std::string, size_t> pick_winners(const std::vector<KeyedMod>& mods)
{
	std::map<std::string, size_t> last;
	for (size_t i = 0; i < mods.size(); i++) {
		for (const std::string& k : mods[i].keys) last[k] = i;
	}
	for (auto it = last.begin(); it != last.end();) {
		const std::string arc = archive_of_key(it->first);
		if (!arc.empty()) {
			const auto whole = last.find(arc); // a loose key: never erased here
			if (whole != last.end() && whole->second > it->second) {
				it = last.erase(it); // the later whole-archive replacement wins
				continue;
			}
		}
		++it;
	}
	return last;
}

std::vector<Conflict> find_conflicts(const std::vector<KeyedMod>& mods)
{
	std::map<std::string, std::vector<size_t>> byKey; // key -> indices of the mods with it, in order
	for (size_t i = 0; i < mods.size(); i++) {
		for (const std::string& k : mods[i].keys) byKey[k].push_back(i);
	}
	std::vector<Conflict> out;
	for (const auto& [key, holders] : byKey) {
		std::vector<size_t> ids = holders;
		std::string archiveMod;
		if (const std::string arc = archive_of_key(key); !arc.empty()) {
			if (const auto whole = byKey.find(arc); whole != byKey.end()) {
				const size_t r = whole->second.back(); // the archive replacement that is used
				if (std::find(ids.begin(), ids.end(), r) == ids.end()) {
					ids.insert(std::upper_bound(ids.begin(), ids.end(), r), r);
					archiveMod = mods[r].id;
				}
			}
		}
		if (ids.size() < 2) continue;
		Conflict c;
		c.key        = key;
		c.winner     = mods[ids.back()].id;
		c.archiveMod = archiveMod;
		for (size_t i = ids.size() - 1; i-- > 0;) c.losers.push_back(mods[ids[i]].id);
		out.push_back(std::move(c));
	}
	return out; // std::map: sorted by key
}

const char* const kBadFolderName =
    "The folder name is not a mod id (1 to 64 characters of a-z, 0-9 and -), so this mod can't load. "
    "Rename the folder to the mod's id.";

std::vector<std::pair<std::string, Issue>> off_notices(const Scan& scan)
{
	std::vector<std::pair<std::string, Issue>> out;
	for (const ModInfo& m : scan.mods) {
		if (m.enabled) continue;
		for (const Issue& i : m.issues) {
			const bool missing = !m.present && m.listed;
			const bool badName = m.present && !m.listed && i.path.empty() && i.message == kBadFolderName;
			if (missing || badName) out.push_back({m.id, i});
		}
	}
	return out;
}

std::string hex_of(const Hash& hash, size_t digits)
{
	static const char* const kDigits = "0123456789abcdef";
	std::string s;
	for (const uint8_t b : hash) {
		s += kDigits[b >> 4];
		s += kDigits[b & 15];
	}
	if (digits < s.size()) s.resize(digits);
	return s;
}

void finish_set(FullScan& full)
{
	Scan& scan = full.scan;
	uint64_t total = 0;
	std::vector<KeyedMod> keyed;
	std::vector<SetMod> set;
	scan.enabledValid = 0;
	for (size_t i = 0; i < scan.mods.size(); i++) {
		ModInfo& mod = scan.mods[i];
		if (!(mod.enabled && mod.present && mod.valid)) continue;
		if (total + mod.totalBytes > kMaxAllModsBytes) {
			add(mod.issues, Severity::Error, "", "Too much mod data: the limit is 512 MiB for all mods together.");
			mod.valid = false;
			continue;
		}
		total += mod.totalBytes;
		scan.enabledValid++;
		keyed.push_back({mod.id, mod.keys});
		SetMod sm{mod.id, mod.version, {}};
		for (const FileRec& f : full.files[i]) sm.files.push_back({f.key, f.size, f.sha});
		set.push_back(std::move(sm));
	}
	scan.conflicts = find_conflicts(keyed);
	scan.setBytes  = serialize_set(set);
	scan.modsHash  = mods_hash_of(scan.setBytes, scan.enabledValid);
}

FullScan scan_dir(const fs::path& dir, std::string_view buildVersion, const ScanOptions& options)
{
	FullScan full;
	Scan& scan = full.scan;
	if (options.off) {
		scan.off = true;
		return full;
	}
	scan.dir = dir;
	try {
		std::error_code ec;
		if (dir.empty() || !fs::is_directory(dir, ec)) return full;
		scan.exists = true;

		// Mod folders: not starting with '.', directories (a folder may itself be a symlink).
		std::vector<std::string> folders;
		for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
			const std::string name = it->path().filename().string();
			if (name.empty() || name[0] == '.') continue;
			std::error_code ec2;
			if (!fs::is_directory(it->path(), ec2)) continue;
			folders.push_back(name);
		}
		if (ec) add(scan.issues, Severity::Error, "", "The mods folder could not be read completely.");
		std::sort(folders.begin(), folders.end());
		if (folders.size() > kMaxModFolders) {
			add(scan.issues, Severity::Warning, "",
			    "The mods folder has more than 256 folders; only the first 256 by name are read.");
			folders.resize(kMaxModFolders);
		}

		LoadOrder order;
		const fs::path orderPath = dir / "load_order.txt";
		if (fs::exists(fs::symlink_status(orderPath, ec))) {
			std::string text;
			const uintmax_t size = fs::file_size(orderPath, ec);
			if (!ec && size > kLoadOrderMaxBytes) {
				order = parse_load_order(std::string(kLoadOrderMaxBytes + 1, '#'));
			} else if (ec || !read_file(orderPath, kLoadOrderMaxBytes, &text)) {
				add(scan.issues, Severity::Error, "load_order.txt", "load_order.txt could not be read, so no mod is on.");
			} else {
				order = parse_load_order(text);
			}
			scan.issues.insert(scan.issues.end(), order.issues.begin(), order.issues.end());
		}

		std::vector<bool> used(folders.size(), false);
		for (const OrderEntry& e : order.entries) {
			ModInfo mod;
			mod.id      = e.id;
			mod.name    = e.id;
			mod.listed  = true;
			mod.enabled = e.enabled;
			mod.dir     = dir / e.id;
			const auto it = std::lower_bound(folders.begin(), folders.end(), e.id);
			if (it != folders.end() && *it == e.id) {
				used[it - folders.begin()] = true;
			} else {
				mod.present = false;
				add(mod.issues, Severity::Warning, "", "load_order.txt lists this mod, but its folder is missing.");
			}
			scan.mods.push_back(std::move(mod));
		}
		for (size_t i = 0; i < folders.size(); i++) {
			if (used[i]) continue;
			ModInfo mod;
			mod.id   = strip_controls(folders[i]);
			mod.name = mod.id;
			mod.dir  = dir / folders[i];
			if (!valid_id(folders[i])) add(mod.issues, Severity::Warning, "", kBadFolderName);
			scan.mods.push_back(std::move(mod));
		}

		full.files.resize(scan.mods.size());
		uint64_t offBudget = kMaxAllModsBytes;
		for (size_t i = 0; i < scan.mods.size(); i++) {
			ModInfo& mod = scan.mods[i];
			if (!mod.present) continue;
			check_mod(mod, full.files[i], buildVersion, options.readAll || mod.enabled,
			          mod.enabled ? nullptr : &offBudget);
			mod.valid = !has_error(mod.issues);
		}
		finish_set(full);
	} catch (...) {
		FullScan failed;
		failed.scan.dir    = dir;
		failed.scan.exists = true;
		add(failed.scan.issues, Severity::Error, "", "The mods folder could not be read.");
		return failed;
	}
	return full;
}

} // namespace mppm::mods::pure
