// Data-mod overlay, milestone M5.2: in-archive overrides v1 (design section 6, plan 7a "Second step").
//
// check() and install() of port/mods/archive.hpp read each targeted archive through Aurora's DVD API
// (so a loose overlay that replaces the whole archive is what they see), Yaz0-decode it, index it
// (port/mods/pure/archive_index.*) and build the registry behind p2_mods_arc_* (port/include/p2_mods.h),
// which port/assets/rarc.cpp consults while it converts an archive's tables. The registry is built once,
// before the game starts, and never changes afterwards, so the readers take no lock.
#include "archive.hpp"
#include "pure/archive_index.hpp"

#include "p2_mods.h"

#include <aurora/dvd.h>
#include <dolphin/dvd.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>

namespace mppm::mods::archive {

namespace {

namespace ai = mppm::mods::archive_index;

constexpr size_t kMaxSets = 254;            // set ids 1..254 fit the info block's padding byte
constexpr uint32_t kMaxArchiveFile = 64u << 20; // bytes of one archive file on the disc

enum class ReadStatus { Ok, NotOnDisc, NotArchive };

struct Loaded {
    ReadStatus status = ReadStatus::NotOnDisc;
    std::string why; // reason when NotArchive
    ai::Index index;
    uint32_t infoEnd = 0; // string table offset + length in the info block (the info_key range)
};

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

// Reads `path` (normalized, absolute) once and indexes it.
Loaded load_archive(const std::string& path)
{
    Loaded out;
    DVDFileInfo fi{};
    if (!DVDOpen(path.c_str(), &fi)) {
        out.status = ReadStatus::NotOnDisc;
        return out;
    }
    const uint32_t len = fi.length;
    out.status = ReadStatus::NotArchive;
    if (len < 0x20 || len > kMaxArchiveFile) {
        DVDClose(&fi);
        out.why = len < 0x20 ? "too small" : "larger than 64 MiB";
        return out;
    }
    std::vector<uint8_t> raw(len);
    const s32 got = DVDReadPrio(&fi, raw.data(), static_cast<s32>(len), 0, 2);
    DVDClose(&fi);
    if (got < static_cast<s32>(len)) {
        out.why = "read error";
        return out;
    }
    std::vector<uint8_t> decoded;
    const std::vector<uint8_t>* arc = &raw;
    if (ai::is_yaz0(raw.data(), raw.size())) {
        std::string err;
        if (!ai::yaz0_decode(raw.data(), raw.size(), decoded, &err)) {
            out.why = "Yaz0: " + err;
            return out;
        }
        arc = &decoded;
    }
    out.index = ai::rarc_index(arc->data(), arc->size());
    if (!out.index.ok) {
        out.why = out.index.error;
        return out;
    }
    const uint8_t* info = arc->data() + be32(arc->data() + 0x08);
    out.infoEnd = be32(info + 0x14) + be32(info + 0x10);
    out.status = ReadStatus::Ok;
    return out;
}

// The distinct archives of `entries`, in order of first appearance: archive path -> entry indices.
std::vector<std::pair<std::string, std::vector<size_t>>> group(const std::vector<Entry>& entries)
{
    std::vector<std::pair<std::string, std::vector<size_t>>> groups;
    std::map<std::string, size_t> at;
    for (size_t i = 0; i < entries.size(); i++) {
        auto [it, added] = at.emplace(entries[i].archivePath, groups.size());
        if (added) {
            groups.push_back({entries[i].archivePath, {}});
        }
        groups[it->second].second.push_back(i);
    }
    return groups;
}

// ---- registry --------------------------------------------------------------------------------

struct SetStore {
    std::string archivePath;
    std::vector<std::string> inner; // filled completely before `files` points into it
    std::vector<P2ModsArcFile> files;
    std::unique_ptr<std::atomic<uint8_t>[]> noted;
    uint64_t key = 0;
    uint32_t infoEnd = 0;
    P2ModsArcSet set{};
};

std::vector<std::unique_ptr<SetStore>> g_sets; // index = id - 1; written by install() only
std::atomic<int> g_any{0};
bool g_installed = false;

const char* mode_name(int mode)
{
    switch (mode) {
    case 1: return "mem";
    case 2: return "aram";
    case 3: return "dvd";
    case 4: return "comp";
    default: return "unknown";
    }
}

} // namespace

std::vector<Problem> check(const std::vector<Entry>& entries)
{
    std::vector<Problem> problems;
    const auto groups = group(entries);
    for (size_t g = 0; g < groups.size(); g++) {
        const std::string& path = groups[g].first;
        const std::vector<size_t>& members = groups[g].second;
        if (g >= kMaxSets) {
            for (size_t e : members) {
                problems.push_back({e, "More than 254 archives have overrides, so the overrides of " + path + " cannot be used."});
            }
            continue;
        }
        const Loaded loaded = load_archive(path);
        if (loaded.status == ReadStatus::NotOnDisc) {
            for (size_t e : members) {
                problems.push_back({e, "The archive " + path + " is not on the disc."});
            }
            continue;
        }
        if (loaded.status == ReadStatus::NotArchive) {
            for (size_t e : members) {
                problems.push_back({e, "The file " + path + " is not an archive (" + loaded.why + ")."});
            }
            continue;
        }
        for (size_t e : members) {
            const std::string& inner = entries[e].innerPath;
            const auto it = loaded.index.paths.find(inner);
            if (it == loaded.index.paths.end()) {
                // archives/outer.szs/inner.arc/file: the split is at the first archive, so a file
                // inside a nested archive arrives as "inner.arc/file". Say so instead of "add".
                std::string nested;
                for (size_t slash = inner.find('/'); slash != std::string::npos; slash = inner.find('/', slash + 1)) {
                    const auto pre = loaded.index.paths.find(inner.substr(0, slash));
                    if (pre != loaded.index.paths.end() && !pre->second.isFolder) {
                        nested = inner.substr(0, slash);
                        break;
                    }
                }
                const auto ends = [&](const char* ext) {
                    return nested.size() > 4 && nested.compare(nested.size() - 4, 4, ext) == 0;
                };
                if (!nested.empty() && (ends(".szs") || ends(".arc"))) {
                    problems.push_back({e, "The file " + nested + " in the archive " + path +
                                               " is itself an archive. Archives inside archives are not supported"
                                               " yet; replace the whole " + path + " as a loose file instead."});
                } else if (!nested.empty()) {
                    problems.push_back({e, "The path " + nested + " in the archive " + path + " is a file, not a folder."});
                } else {
                    problems.push_back({e, "The archive " + path + " has no file " + inner +
                                               ". Adding files to an archive is not supported yet."});
                }
            } else if (it->second.isFolder) {
                problems.push_back({e, "The path " + inner + " in the archive " + path + " is a folder, not a file."});
            }
        }
    }
    return problems;
}

void install(const std::vector<Entry>& entries)
{
    if (g_installed) {
        std::fprintf(stderr, "mppm: mods: archive overrides are already installed; ignoring a second install\n");
        return;
    }
    g_installed = true;
    const auto groups = group(entries);
    for (size_t g = 0; g < groups.size() && g_sets.size() < kMaxSets; g++) {
        const std::string& path = groups[g].first;
        const Loaded loaded = load_archive(path);
        if (loaded.status != ReadStatus::Ok) {
            std::fprintf(stderr, "mppm: mods: archive %s could not be read again; its overrides are not used\n", path.c_str());
            continue;
        }
        struct Pick {
            uint32_t index;
            size_t entry;
        };
        std::vector<Pick> picks;
        for (size_t e : groups[g].second) {
            const auto it = loaded.index.paths.find(entries[e].innerPath);
            if (it == loaded.index.paths.end() || it->second.isFolder || entries[e].data == nullptr) {
                continue; // check() reported it
            }
            picks.push_back({it->second.fileIndex, e});
        }
        if (picks.empty()) {
            continue;
        }
        std::sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) { return a.index < b.index; });
        auto st = std::make_unique<SetStore>();
        st->archivePath = path;
        st->key = loaded.index.key;
        st->infoEnd = loaded.infoEnd;
        st->inner.reserve(picks.size());
        for (const Pick& p : picks) {
            st->inner.push_back(entries[p.entry].innerPath);
        }
        st->files.reserve(picks.size());
        for (size_t k = 0; k < picks.size(); k++) {
            const Entry& en = entries[picks[k].entry];
            st->files.push_back({picks[k].index, en.size, en.data, st->inner[k].c_str()});
        }
        st->noted = std::make_unique<std::atomic<uint8_t>[]>(picks.size());
        for (size_t k = 0; k < picks.size(); k++) {
            st->noted[k].store(0, std::memory_order_relaxed);
        }
        for (const auto& prev : g_sets) {
            if (prev->key == st->key) {
                std::fprintf(stderr, "mppm: mods: warning: archives %s and %s have identical tables; buffer mounts use the first\n",
                             prev->archivePath.c_str(), path.c_str());
                break;
            }
        }
        st->set.id = static_cast<uint32_t>(g_sets.size() + 1);
        st->set.archivePath = st->archivePath.c_str();
        st->set.entryNum = DVDConvertPathToEntrynum(path.c_str());
        st->set.count = static_cast<uint32_t>(st->files.size());
        st->set.files = st->files.data();
        g_sets.push_back(std::move(st));
    }
    g_any.store(g_sets.empty() ? 0 : 1, std::memory_order_release);
}

} // namespace mppm::mods::archive

using mppm::mods::archive::g_any;
using mppm::mods::archive::g_sets;

extern "C" {

int p2_mods_arc_any(void) { return g_any.load(std::memory_order_acquire); }

const P2ModsArcSet* p2_mods_arc_find(const void* rawInfo, int32_t entryNum)
{
    if (rawInfo == nullptr || !p2_mods_arc_any()) {
        return nullptr;
    }
    // Hash only a range some installed set has: the info block is trusted no further than that.
    const uint8_t* p = static_cast<const uint8_t*>(rawInfo);
    const uint32_t end = mppm::mods::archive::be32(p + 0x14) + mppm::mods::archive::be32(p + 0x10);
    uint64_t key = 0;
    bool hashed = false;
    const P2ModsArcSet* first = nullptr;
    for (const auto& st : g_sets) {
        if (st->infoEnd != end) {
            continue;
        }
        if (!hashed) {
            key = mppm::mods::archive_index::info_key(rawInfo, end);
            hashed = true;
        }
        if (key == 0 || st->key != key) {
            continue;
        }
        if (st->set.entryNum == entryNum) {
            return &st->set;
        }
        if (first == nullptr) {
            first = &st->set;
        }
    }
    // No set has this entryNum. A DVD, ARAM, Comp or Mem-by-entry mount carries the real disc entry,
    // so the match is a byte-identical copy of the overridden archive under another path (the disc
    // has hundreds, e.g. /user/Abe/Pellet/{jpn,pal,us}/): leave it alone. Only a buffer mount, whose
    // mEntryNum is not a disc entry (a truncated pointer), takes the content match.
    if (entryNum >= 0 && entryNum < aurora_dvd_base_entry_count()) {
        return nullptr;
    }
    return first;
}

const P2ModsArcSet* p2_mods_arc_get(uint32_t id)
{
    if (id == 0 || !p2_mods_arc_any() || id > g_sets.size()) {
        return nullptr;
    }
    return &g_sets[id - 1]->set;
}

void p2_mods_arc_note(const P2ModsArcSet* set, uint32_t fileIndexInSet, int mountMode, int ok)
{
    if (set == nullptr || set->id == 0 || set->id > g_sets.size()) {
        return;
    }
    auto& st = *g_sets[set->id - 1];
    if (&st.set != set || fileIndexInSet >= st.set.count) {
        return;
    }
    if (st.noted[fileIndexInSet].exchange(1, std::memory_order_acq_rel) != 0) {
        return;
    }
    const P2ModsArcFile& f = st.files[fileIndexInSet];
    if (ok) {
        std::fprintf(stderr, "mppm: mods: archive override %s:%s applied (%s, %" PRIu32 " bytes)\n", st.archivePath.c_str(),
                     f.innerPath, mppm::mods::archive::mode_name(mountMode), f.size);
    } else {
        std::fprintf(stderr, "mppm: mods: archive override %s:%s failed (%s): out of memory\n", st.archivePath.c_str(),
                     f.innerPath, mppm::mods::archive::mode_name(mountMode));
    }
}

} // extern "C"
