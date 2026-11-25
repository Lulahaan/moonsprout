// p2mods_selftest (M5.2): the pure part of in-archive overrides (port/mods/pure/archive_index.*).
// Every archive here is built byte by byte from made-up names and contents: no Nintendo data.
#include "test.hpp"

#include "../../port/mods/pure/archive_index.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace {

namespace ai = mppm::mods::archive_index;

void put32(std::vector<uint8_t>& v, size_t at, uint32_t x)
{
    v[at] = uint8_t(x >> 24);
    v[at + 1] = uint8_t(x >> 16);
    v[at + 2] = uint8_t(x >> 8);
    v[at + 3] = uint8_t(x);
}

void put16(std::vector<uint8_t>& v, size_t at, uint16_t x)
{
    v[at] = uint8_t(x >> 8);
    v[at + 1] = uint8_t(x);
}

// Layout of the synthetic archive (offsets of the info block are relative to 0x20):
//   root "Root" (folder 0): "Alpha.TXT" (file 0), "SubDir" (folder link 1), "." (2), ".." (3)
//   folder 1 "SubDir": "Beta.bin" (file 4), "." (5), ".." (6)
// Folder table at info+0x20 (2 x 0x10), file table at info+0x40 (7 x 0x14), strings at info+0xCC.
struct Synth {
    std::vector<uint8_t> bytes;
    size_t info = 0x20;
    size_t dirTable = 0;
    size_t fileTable = 0;
    size_t strTable = 0;
    uint32_t strLen = 0;
};

Synth make_rarc()
{
    Synth s;
    const char names[] = "Root\0Alpha.TXT\0SubDir\0.\0..\0Beta.bin\0";
    const uint32_t nRoot = 0, nAlpha = 5, nSub = 15, nDot = 22, nDotDot = 24, nBeta = 27;
    const uint32_t strLen = sizeof(names); // includes the final NUL twice; padded below
    const uint32_t dirOff = 0x20, fileOff = 0x40, strOff = 0x40 + 7 * 0x14; // 0xCC
    const uint32_t strLenPadded = (strLen + 3) & ~3u;
    const uint32_t infoLen = strOff + strLenPadded;
    const std::string alpha = "hello made-up text\n";
    const std::string beta = "\x01\x02\x03\x04";
    const uint32_t dataOff = (infoLen + 31) & ~31u; // file data, relative to the info block
    const uint32_t alphaAt = 0, betaAt = 32;
    s.bytes.assign(0x20 + dataOff + 64, 0);
    auto& b = s.bytes;
    std::memcpy(b.data(), "RARC", 4);
    put32(b, 0x04, uint32_t(b.size()));
    put32(b, 0x08, 0x20);
    put32(b, 0x0C, dataOff);
    put32(b, 0x10, 64);
    const size_t I = s.info;
    put32(b, I + 0x00, 2);
    put32(b, I + 0x04, dirOff);
    put32(b, I + 0x08, 7);
    put32(b, I + 0x0C, fileOff);
    put32(b, I + 0x10, strLenPadded);
    put32(b, I + 0x14, strOff);
    put16(b, I + 0x18, 7);
    b[I + 0x1A] = 1;
    // folders: type, name offset, hash, count, first
    std::memcpy(&b[I + dirOff], "ROOT", 4);
    put32(b, I + dirOff + 0x04, nRoot);
    put16(b, I + dirOff + 0x0A, 4);
    put32(b, I + dirOff + 0x0C, 0);
    std::memcpy(&b[I + dirOff + 0x10], "SUBD", 4);
    put32(b, I + dirOff + 0x14, nSub);
    put16(b, I + dirOff + 0x1A, 3);
    put32(b, I + dirOff + 0x1C, 4);
    auto file = [&](uint32_t i, uint16_t id, uint32_t flags, uint32_t name, uint32_t off, uint32_t size) {
        const size_t f = I + fileOff + i * 0x14;
        put16(b, f, id);
        put32(b, f + 0x04, (flags << 24) | name);
        put32(b, f + 0x08, off);
        put32(b, f + 0x0C, size);
    };
    file(0, 0, 0x11, nAlpha, alphaAt, uint32_t(alpha.size()));
    file(1, 0xFFFF, 0x02, nSub, 1, 0x10);
    file(2, 0xFFFF, 0x02, nDot, 0, 0x10);
    file(3, 0xFFFF, 0x02, nDotDot, 0xFFFFFFFF, 0x10);
    file(4, 1, 0x11, nBeta, betaAt, uint32_t(beta.size()));
    file(5, 0xFFFF, 0x02, nDot, 1, 0x10);
    file(6, 0xFFFF, 0x02, nDotDot, 0, 0x10);
    std::memcpy(&b[I + strOff], names, sizeof(names));
    std::memcpy(&b[0x20 + dataOff + alphaAt], alpha.data(), alpha.size());
    std::memcpy(&b[0x20 + dataOff + betaAt], beta.data(), beta.size());
    s.dirTable = I + dirOff;
    s.fileTable = I + fileOff;
    s.strTable = I + strOff;
    s.strLen = strLenPadded;
    return s;
}

std::vector<uint8_t> bytes_of(const char* s) { return std::vector<uint8_t>(s, s + std::strlen(s)); }

std::vector<uint8_t> yaz0_header(uint32_t size)
{
    std::vector<uint8_t> v(16, 0);
    std::memcpy(v.data(), "Yaz0", 4);
    put32(v, 4, size);
    return v;
}

} // namespace

MODS_TEST(archive_rarc_index_synthetic)
{
    const Synth s = make_rarc();
    const ai::Index idx = ai::rarc_index(s.bytes.data(), s.bytes.size());
    MODS_CHECK(idx.ok);
    MODS_CHECK_EQ(idx.paths.size(), size_t(3));
    const auto a = idx.paths.find("alpha.txt");
    MODS_CHECK(a != idx.paths.end());
    if (a != idx.paths.end()) {
        MODS_CHECK_EQ(a->second.fileIndex, 0u);
        MODS_CHECK(!a->second.isFolder);
    }
    const auto d = idx.paths.find("subdir");
    MODS_CHECK(d != idx.paths.end() && d->second.isFolder && d->second.fileIndex == 1u);
    const auto bt = idx.paths.find("subdir/beta.bin");
    MODS_CHECK(bt != idx.paths.end() && !bt->second.isFolder && bt->second.fileIndex == 4u);
    // The root folder's name is not part of an inner path, and "." / ".." are skipped.
    MODS_CHECK(idx.paths.count("root/alpha.txt") == 0);
    MODS_CHECK(idx.paths.count(".") == 0 && idx.paths.count("subdir/..") == 0);
    MODS_CHECK(idx.key != 0);
    MODS_CHECK_EQ(idx.key, ai::info_key(s.bytes.data() + 0x20, s.bytes.size() - 0x20));
}

MODS_TEST(archive_inner_path_case)
{
    const Synth s = make_rarc();
    const ai::Index idx = ai::rarc_index(s.bytes.data(), s.bytes.size());
    // Names are stored lower case, so a lookup of any spelling lower-cases its query first.
    MODS_CHECK(idx.paths.count("alpha.txt") == 1);
    MODS_CHECK(idx.paths.count("Alpha.TXT") == 0);
    MODS_CHECK(idx.paths.count("subdir/beta.bin") == 1);
    MODS_CHECK(idx.paths.count("subdir/gamma.bin") == 0);
}

MODS_TEST(archive_folder_is_not_a_file)
{
    const Synth s = make_rarc();
    const ai::Index idx = ai::rarc_index(s.bytes.data(), s.bytes.size());
    const auto d = idx.paths.find("subdir");
    MODS_CHECK(d != idx.paths.end());
    if (d != idx.paths.end()) {
        MODS_CHECK(d->second.isFolder); // archive::check refuses an override of it
    }
}

MODS_TEST(archive_rarc_malformed)
{
    const Synth good = make_rarc();
    // Not an archive / truncated header.
    MODS_CHECK(!ai::rarc_index(nullptr, 0).ok);
    MODS_CHECK(!ai::rarc_index(good.bytes.data(), 0x1F).ok);
    MODS_CHECK(!ai::rarc_index(good.bytes.data(), 0x30).ok); // info block cut short
    {
        std::vector<uint8_t> b = good.bytes;
        b[0] = 'X';
        MODS_CHECK(!ai::rarc_index(b.data(), b.size()).ok);
    }
    {
        std::vector<uint8_t> b = good.bytes;
        put32(b, 0x08, uint32_t(b.size())); // header length past the end
        MODS_CHECK(!ai::rarc_index(b.data(), b.size()).ok);
    }
    {
        std::vector<uint8_t> b = good.bytes;
        put32(b, good.info + 0x0C, 0x7FFFFFF0); // file table offset past the end
        MODS_CHECK(!ai::rarc_index(b.data(), b.size()).ok);
    }
    {
        std::vector<uint8_t> b = good.bytes;
        put32(b, good.info + 0x08, 0xFFFFFFFF); // file count over the limit
        MODS_CHECK(!ai::rarc_index(b.data(), b.size()).ok);
    }
    {
        std::vector<uint8_t> b = good.bytes;
        put32(b, good.info + 0x10, 0x10000); // string table past the end
        MODS_CHECK(!ai::rarc_index(b.data(), b.size()).ok);
    }
    {
        std::vector<uint8_t> b = good.bytes;
        put32(b, good.fileTable + 0x04, (0x11u << 24) | 0xFFFFFF); // name outside the string table
        MODS_CHECK(!ai::rarc_index(b.data(), b.size()).ok);
    }
    {
        std::vector<uint8_t> b = good.bytes;
        put32(b, good.dirTable + 0x10 + 0x0C, 6); // folder 1 file range runs past the table
        MODS_CHECK(!ai::rarc_index(b.data(), b.size()).ok);
    }
    {
        std::vector<uint8_t> b = good.bytes;
        put32(b, good.fileTable + 1 * 0x14 + 0x08, 9); // folder link to a missing folder
        MODS_CHECK(!ai::rarc_index(b.data(), b.size()).ok);
    }
    {
        // Cycle: folder 1 links back to the root under a normal name.
        std::vector<uint8_t> b = good.bytes;
        put32(b, good.fileTable + 4 * 0x14 + 0x04, (0x02u << 24) | 5); // "Alpha.TXT" as a folder
        put32(b, good.fileTable + 4 * 0x14 + 0x08, 0);
        const ai::Index idx = ai::rarc_index(b.data(), b.size());
        MODS_CHECK(!idx.ok);
        MODS_CHECK(idx.paths.empty());
        MODS_CHECK(!idx.error.empty());
    }
    {
        // Self-loop: the root's folder link points at the root.
        std::vector<uint8_t> b = good.bytes;
        put32(b, good.fileTable + 1 * 0x14 + 0x08, 0);
        MODS_CHECK(!ai::rarc_index(b.data(), b.size()).ok);
    }
    // Every truncation of the good archive is refused or indexed, never read past the end.
    for (size_t n = 0; n < good.bytes.size(); n++) {
        std::vector<uint8_t> b(good.bytes.begin(), good.bytes.begin() + long(n));
        const ai::Index idx = ai::rarc_index(b.data(), b.size());
        if (n < good.strTable + good.strLen) { // the info block is incomplete
            MODS_CHECK(!idx.ok);
        }
    }
}

MODS_TEST(archive_yaz0_literals)
{
    std::vector<uint8_t> src = yaz0_header(10);
    const char* text = "0123456789";
    src.push_back(0xFF);
    for (int i = 0; i < 8; i++) src.push_back(uint8_t(text[i]));
    src.push_back(0xC0);
    src.push_back(uint8_t(text[8]));
    src.push_back(uint8_t(text[9]));
    std::vector<uint8_t> out;
    std::string err;
    MODS_CHECK(ai::is_yaz0(src.data(), src.size()));
    MODS_CHECK(ai::yaz0_decode(src.data(), src.size(), out, &err));
    MODS_CHECK(out == bytes_of(text));
}

MODS_TEST(archive_yaz0_backref)
{
    // "abcabcabcabcX": 3 literals, a back reference (distance 3, length 9), one literal.
    std::vector<uint8_t> src = yaz0_header(13);
    src.push_back(0xE8); // 1110 1000: lit lit lit ref lit
    src.push_back('a');
    src.push_back('b');
    src.push_back('c');
    src.push_back(uint8_t(((9 - 2) << 4) | 0x0)); // count 9, distance-1 high nibble 0
    src.push_back(0x02);                          // distance-1 = 2
    src.push_back('X');
    std::vector<uint8_t> out;
    MODS_CHECK(ai::yaz0_decode(src.data(), src.size(), out));
    MODS_CHECK(out == bytes_of("abcabcabcabcX"));
    // Long form (count byte + 0x12): 1 literal then 0x12 + 2 = 20 copies of it.
    std::vector<uint8_t> lng = yaz0_header(21);
    lng.push_back(0x80);
    lng.push_back('z');
    lng.push_back(0x00);
    lng.push_back(0x00); // distance 1
    lng.push_back(0x02); // count 0x14
    MODS_CHECK(ai::yaz0_decode(lng.data(), lng.size(), out));
    MODS_CHECK(out == std::vector<uint8_t>(21, 'z'));
}

MODS_TEST(archive_yaz0_malformed)
{
    std::vector<uint8_t> out;
    std::string err;
    // Truncated header.
    const std::vector<uint8_t> shortHdr = {'Y', 'a', 'z', '0', 0, 0, 0, 4};
    MODS_CHECK(!ai::yaz0_decode(shortHdr.data(), shortHdr.size(), out, &err));
    // Back reference before the start of the output.
    std::vector<uint8_t> bad = yaz0_header(8);
    bad.push_back(0x80);
    bad.push_back('q');
    bad.push_back(0x10); // count 3, distance 1 + 0x... high nibble 0
    bad.push_back(0x05); // distance 6 > 1 byte produced
    MODS_CHECK(!ai::yaz0_decode(bad.data(), bad.size(), out, &err));
    MODS_CHECK(out.empty());
    MODS_CHECK(!err.empty());
    // Stream ends before the decoded size.
    std::vector<uint8_t> cut = yaz0_header(10);
    cut.push_back(0xFF);
    cut.push_back('a');
    MODS_CHECK(!ai::yaz0_decode(cut.data(), cut.size(), out, &err));
    // Decoded size over 64 MiB.
    std::vector<uint8_t> big = yaz0_header((64u << 20) + 1);
    big.push_back(0xFF);
    MODS_CHECK(!ai::yaz0_decode(big.data(), big.size(), out, &err));
    // Not Yaz0 at all.
    std::vector<uint8_t> rarc(32, 0);
    std::memcpy(rarc.data(), "RARC", 4);
    MODS_CHECK(!ai::is_yaz0(rarc.data(), rarc.size()));
    MODS_CHECK(!ai::yaz0_decode(rarc.data(), rarc.size(), out, &err));
}

MODS_TEST(archive_yaz0_of_rarc)
{
    // A literal-only Yaz0 stream of the synthetic archive decodes and indexes like the original.
    const Synth s = make_rarc();
    std::vector<uint8_t> src = yaz0_header(uint32_t(s.bytes.size()));
    for (size_t i = 0; i < s.bytes.size(); i += 8) {
        src.push_back(0xFF);
        for (size_t k = i; k < i + 8 && k < s.bytes.size(); k++) src.push_back(s.bytes[k]);
    }
    std::vector<uint8_t> out;
    MODS_CHECK(ai::yaz0_decode(src.data(), src.size(), out));
    MODS_CHECK(out == s.bytes);
    MODS_CHECK(ai::rarc_index(out.data(), out.size()).ok);
}

MODS_TEST(archive_info_key)
{
    MODS_CHECK_EQ(ai::fnv1a64(nullptr, 0), 0xcbf29ce484222325ull);
    const uint8_t a = 'a';
    MODS_CHECK_EQ(ai::fnv1a64(&a, 1), 0xaf63dc4c8601ec8cull);
    const uint8_t foobar[] = {'f', 'o', 'o', 'b', 'a', 'r'};
    MODS_CHECK_EQ(ai::fnv1a64(foobar, 6), 0x85944171f73967e8ull);

    const Synth s = make_rarc();
    const uint8_t* info = s.bytes.data() + 0x20;
    const size_t avail = s.bytes.size() - 0x20;
    const size_t end = (s.strTable - s.info) + s.strLen;
    MODS_CHECK_EQ(ai::info_key(info, avail), ai::fnv1a64(info, end));
    MODS_CHECK_EQ(ai::info_key(info, end), ai::fnv1a64(info, end)); // exactly enough
    MODS_CHECK_EQ(ai::info_key(info, end - 1), 0ull);              // end > avail
    MODS_CHECK_EQ(ai::info_key(nullptr, avail), 0ull);
    MODS_CHECK_EQ(ai::info_key(info, 0x1F), 0ull);
    {
        std::vector<uint8_t> b(s.bytes.begin() + 0x20, s.bytes.end());
        put32(b, 0x14, 0x08);
        put32(b, 0x10, 0x08); // end 0x10 < 0x20
        MODS_CHECK_EQ(ai::info_key(b.data(), b.size()), 0ull);
        put32(b, 0x14, 0x00800000);
        put32(b, 0x10, 0x00800001); // end > 16 MiB
        MODS_CHECK_EQ(ai::info_key(b.data(), b.size()), 0ull);
        put32(b, 0x14, 0xFFFFFFFF);
        put32(b, 0x10, 0x00000040); // wraps in 32 bits, not in 64
        MODS_CHECK_EQ(ai::info_key(b.data(), b.size()), 0ull);
    }
    {
        // A changed name changes the key; a changed file byte outside the info block does not.
        std::vector<uint8_t> b = s.bytes;
        b[s.strTable + 6] = 'X';
        MODS_CHECK(ai::info_key(b.data() + 0x20, avail) != ai::info_key(info, avail));
        std::vector<uint8_t> c = s.bytes;
        c[c.size() - 1] ^= 0x55;
        MODS_CHECK_EQ(ai::info_key(c.data() + 0x20, avail), ai::info_key(info, avail));
    }
}
