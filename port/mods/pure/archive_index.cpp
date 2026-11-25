// Data-mod overlay, milestone M5.2: Yaz0 decoder, RARC index and info-block key (pure, bounded).
#include "archive_index.hpp"

#include <cstring>

namespace mppm::mods::archive_index {

namespace {

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }

constexpr size_t kInfoSize = 0x20;      // SArcDataInfo
constexpr size_t kDirEntrySize = 0x10;  // SDIDirEntry
constexpr size_t kFileEntrySize = 0x14; // on-disc SDIFileEntry
constexpr size_t kMaxInnerPath = 1024;  // bytes of one lower-case inner path

bool fail(std::string* error, const char* why)
{
    if (error != nullptr) {
        *error = why;
    }
    return false;
}

struct Walker {
    const uint8_t* info = nullptr; // start of the info block
    uint32_t numDirs = 0;
    uint32_t numFiles = 0;
    const uint8_t* dirs = nullptr;
    const uint8_t* files = nullptr;
    const char* str = nullptr;
    uint32_t strLen = 0;
    std::vector<uint8_t> visited;
    Index* out = nullptr;

    // The NUL-terminated name at `off` in the string table, or false when it does not end in it.
    bool name(uint32_t off, std::string& s) const
    {
        if (off >= strLen) {
            return false;
        }
        const void* nul = std::memchr(str + off, 0, strLen - off);
        if (nul == nullptr) {
            return false;
        }
        s.assign(str + off, static_cast<const char*>(nul));
        return true;
    }

    bool walk(uint32_t dir, const std::string& prefix, int depth)
    {
        if (depth > kMaxDepth) {
            out->error = "folders nested too deeply";
            return false;
        }
        if (dir >= numDirs) {
            out->error = "folder index out of range";
            return false;
        }
        if (visited[dir]) {
            out->error = "a folder is reached twice (cycle)";
            return false;
        }
        visited[dir] = 1;
        const uint8_t* d = dirs + size_t(dir) * kDirEntrySize;
        const uint32_t count = be16(d + 0x0A);
        const uint32_t first = be32(d + 0x0C);
        if (first > numFiles || count > numFiles - first) {
            out->error = "folder file range out of range";
            return false;
        }
        std::string nm;
        for (uint32_t i = first; i < first + count; i++) {
            const uint8_t* f = files + size_t(i) * kFileEntrySize;
            const uint32_t flag = be32(f + 0x04);
            const uint32_t flags = flag >> 24;
            if (!name(flag & 0xFFFFFF, nm)) {
                out->error = "file name outside the string table";
                return false;
            }
            const bool isFolder = (flags & 0x02) != 0;
            if (isFolder && (nm == "." || nm == "..")) {
                continue;
            }
            if (nm.empty()) {
                out->error = "empty file name";
                return false;
            }
            for (char& c : nm) {
                if (c >= 'A' && c <= 'Z') {
                    c = char(c - 'A' + 'a');
                }
            }
            std::string path = prefix.empty() ? nm : prefix + "/" + nm;
            if (path.size() > kMaxInnerPath) {
                out->error = "inner path too long";
                return false;
            }
            out->paths.emplace(path, Node{i, isFolder});
            if (isFolder) {
                if (!walk(be32(f + 0x08), path, depth + 1)) {
                    return false;
                }
            }
        }
        return true;
    }
};

} // namespace

uint64_t fnv1a64(const uint8_t* p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

bool is_yaz0(const uint8_t* src, size_t n) { return src != nullptr && n >= 4 && std::memcmp(src, "Yaz0", 4) == 0; }

bool yaz0_decode(const uint8_t* src, size_t n, std::vector<uint8_t>& out, std::string* error)
{
    out.clear();
    if (src == nullptr || n < 16 || std::memcmp(src, "Yaz0", 4) != 0) {
        return fail(error, "not a Yaz0 stream");
    }
    const size_t size = be32(src + 4);
    if (size > kMaxDecoded) {
        return fail(error, "decoded size over the limit");
    }
    out.resize(size);
    size_t s = 16;
    size_t o = 0;
    uint32_t code = 0;
    int bits = 0;
    while (o < size) {
        if (bits == 0) {
            if (s >= n) {
                out.clear();
                return fail(error, "stream ends early");
            }
            code = src[s++];
            bits = 8;
        }
        if (code & 0x80) {
            if (s >= n) {
                out.clear();
                return fail(error, "stream ends early");
            }
            out[o++] = src[s++];
        } else {
            if (n - s < 2) {
                out.clear();
                return fail(error, "stream ends early");
            }
            const uint8_t b1 = src[s++];
            const uint8_t b2 = src[s++];
            const size_t dist = ((size_t(b1 & 0x0F) << 8) | b2) + 1;
            size_t count = b1 >> 4;
            if (count == 0) {
                if (s >= n) {
                    out.clear();
                    return fail(error, "stream ends early");
                }
                count = size_t(src[s++]) + 0x12;
            } else {
                count += 2;
            }
            if (dist > o) {
                out.clear();
                return fail(error, "back reference before the start");
            }
            if (count > size - o) {
                count = size - o; // the last copy may run past the end; the rest is not needed
            }
            for (size_t k = 0; k < count; k++, o++) {
                out[o] = out[o - dist];
            }
        }
        code <<= 1;
        bits--;
    }
    return true;
}

uint64_t info_key(const void* raw, size_t avail)
{
    if (raw == nullptr || avail < kInfoSize) {
        return 0;
    }
    const uint8_t* p = static_cast<const uint8_t*>(raw);
    const uint64_t end = uint64_t(be32(p + 0x14)) + be32(p + 0x10);
    if (end < kInfoSize || end > kMaxInfoKeyBytes || end > avail) {
        return 0;
    }
    return fnv1a64(p, size_t(end));
}

Index rarc_index(const uint8_t* data, size_t n)
{
    Index idx;
    if (data == nullptr || n < 0x20 || std::memcmp(data, "RARC", 4) != 0) {
        idx.error = "not a RARC archive";
        return idx;
    }
    const uint32_t headerLen = be32(data + 0x08);
    if (headerLen < 0x20 || headerLen > n || n - headerLen < kInfoSize) {
        idx.error = "header length out of range";
        return idx;
    }
    const uint8_t* info = data + headerLen;
    const size_t avail = n - headerLen;
    const uint32_t numDirs = be32(info + 0x00);
    const uint32_t dirOff = be32(info + 0x04);
    const uint32_t numFiles = be32(info + 0x08);
    const uint32_t fileOff = be32(info + 0x0C);
    const uint32_t strLen = be32(info + 0x10);
    const uint32_t strOff = be32(info + 0x14);
    if (numDirs == 0 || numDirs > kMaxFolders) {
        idx.error = "folder count out of range";
        return idx;
    }
    if (numFiles > kMaxFileEntries) {
        idx.error = "file count out of range";
        return idx;
    }
    if (dirOff > avail || uint64_t(numDirs) * kDirEntrySize > avail - dirOff) {
        idx.error = "folder table out of range";
        return idx;
    }
    if (fileOff > avail || uint64_t(numFiles) * kFileEntrySize > avail - fileOff) {
        idx.error = "file table out of range";
        return idx;
    }
    if (strOff > avail || strLen > avail - strOff) {
        idx.error = "string table out of range";
        return idx;
    }
    Walker w;
    w.info = info;
    w.numDirs = numDirs;
    w.numFiles = numFiles;
    w.dirs = info + dirOff;
    w.files = info + fileOff;
    w.str = reinterpret_cast<const char*>(info + strOff);
    w.strLen = strLen;
    w.visited.assign(numDirs, 0);
    w.out = &idx;
    if (!w.walk(0, std::string(), 0)) {
        idx.paths.clear();
        return idx;
    }
    idx.key = info_key(info, avail);
    idx.ok = true;
    return idx;
}

} // namespace mppm::mods::archive_index
