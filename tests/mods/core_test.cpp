// p2mods_selftest, core package (M5.2): SHA-256, paths and keys, mod.json, load_order.txt,
// validators, conflicts, the set serialization test vector (design 4.7) and a scan of a temporary
// folder. No Nintendo data: every input is written here.
#include "test.hpp"

#include "pure/load_order.hpp"
#include "pure/manifest.hpp"
#include "pure/modset.hpp"
#include "pure/paths.hpp"
#include "pure/sha256.hpp"
#include "pure/validate.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

using namespace mppm::mods;
using namespace mppm::mods::pure;
namespace fs = std::filesystem;

namespace {

std::string sha_hex(const std::string& s)
{
	return hex_of(Sha256::of(s.data(), s.size()));
}

bool has_issue(const std::vector<Issue>& issues, Severity sev, const std::string& fragment)
{
	for (const Issue& i : issues) {
		if (i.severity == sev && i.message.find(fragment) != std::string::npos) return true;
	}
	return false;
}

int errors(const std::vector<Issue>& issues)
{
	int n = 0;
	for (const Issue& i : issues) n += i.severity == Severity::Error;
	return n;
}

std::string bad(const std::string& entry)
{
	EntryKey k = entry_key(entry);
	return k.ok ? std::string("(accepted)") : k.error;
}

Manifest mf(const std::string& json, const std::string& folder = "my-mod", const std::string& build = "0.1.0-dev+g123")
{
	return parse_manifest(json, folder, build);
}

const char* kGood = R"({"id":"my-mod","version":"1.2.3","files":["files/user/abe/vs/stages.txt"]})";

void write(const fs::path& p, const std::string& text)
{
	fs::create_directories(p.parent_path());
	std::ofstream(p, std::ios::binary) << text;
}

std::string stage_list(int declared, int blocks)
{
	std::string s = std::to_string(declared) + " \t# num stages\n";
	for (int i = 0; i < blocks; i++) s += "# stage\n{\n\t4 \t# version\n\tarena.txt \n\t200.000000 \t# time\n}\n";
	return s;
}

} // namespace

MODS_TEST(sha256_nist_vectors)
{
	MODS_CHECK_EQ(sha_hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	MODS_CHECK_EQ(sha_hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	MODS_CHECK_EQ(sha_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
	              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	const std::string million(1000000, 'a');
	MODS_CHECK_EQ(sha_hex(million), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
	// The same message in uneven pieces.
	Sha256 h;
	size_t pos = 0, step = 1;
	while (pos < million.size()) {
		const size_t n = std::min(step, million.size() - pos);
		h.update(million.data() + pos, n);
		pos += n;
		step = step * 3 % 977 + 1;
	}
	MODS_CHECK_EQ(hex_of(h.finish()), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

MODS_TEST(paths_forbidden)
{
	MODS_CHECK_EQ(bad(""), "The path is empty.");
	MODS_CHECK_EQ(bad("files/../etc/passwd"), "The path contains a . or .. part.");
	MODS_CHECK_EQ(bad("files/./a.txt"), "The path contains a . or .. part.");
	MODS_CHECK_EQ(bad(".."), "The path contains a . or .. part.");
	MODS_CHECK_EQ(bad("/files/a.txt"), "The path must be relative to the mod folder (no leading /).");
	MODS_CHECK_EQ(bad("/etc/hostname"), "The path must be relative to the mod folder (no leading /).");
	MODS_CHECK_EQ(bad("files\\user\\a.txt"), "The path uses a backslash; use / between folders.");
	MODS_CHECK_EQ(bad("files/c:/a.txt"), "The path contains a colon.");
	MODS_CHECK_EQ(bad("files//a.txt"), "The path has an empty folder name (// or a trailing /).");
	MODS_CHECK_EQ(bad("files/user/"), "The path has an empty folder name (// or a trailing /).");
	MODS_CHECK_EQ(bad("files/.hidden"), "A part of the path starts with a dot.");
	MODS_CHECK_EQ(bad("files/user/abc."), "A part of the path ends with a dot.");
	MODS_CHECK_EQ(bad("files/con"), "A part of the path is a reserved device name (con, nul, com1 ...).");
	MODS_CHECK_EQ(bad("files/NUL.txt"), "A part of the path is a reserved device name (con, nul, com1 ...).");
	MODS_CHECK_EQ(bad("files/sub/Com7.bin"), "A part of the path is a reserved device name (con, nul, com1 ...).");
	MODS_CHECK_EQ(bad("files/lpt9"), "A part of the path is a reserved device name (con, nul, com1 ...).");
	MODS_CHECK(entry_key("files/com0.txt").ok);   // not a device
	MODS_CHECK(entry_key("files/console.txt").ok); // not a device
	MODS_CHECK_EQ(bad("files/" + std::string(65, 'a')), "A part of the path is longer than 64 bytes.");
	MODS_CHECK(entry_key("files/" + std::string(64, 'a')).ok);
	std::string deep = "files";
	for (int i = 0; i < 15; i++) deep += "/d";
	MODS_CHECK(entry_key(deep).ok); // 16 parts
	MODS_CHECK_EQ(bad(deep + "/d"), "The path has more than 16 parts.");
	std::string longPath = "files";
	while (longPath.size() <= 240) longPath += "/" + std::string(40, 'x');
	MODS_CHECK_EQ(bad(longPath), "The path is longer than 240 bytes.");
	MODS_CHECK_EQ(bad("files/a b.txt"), "The path contains a character other than letters, digits, '.', '_' and '-'.");
	MODS_CHECK_EQ(bad("files/caf\xc3\xa9.txt"),
	              "The path contains a character other than letters, digits, '.', '_' and '-'.");
	MODS_CHECK_EQ(bad("data/a.txt"), "The path must start with files/ or archives/.");
	MODS_CHECK_EQ(bad("Files/a.txt"), "The path must start with files/ or archives/.");
	MODS_CHECK_EQ(bad("files"), "The path names no file under files/.");
	MODS_CHECK_EQ(bad("archives/enemy/parm/x.txt"), "The path under archives/ names no .szs or .arc archive.");
	MODS_CHECK_EQ(bad("archives/enemy/parm/enemyParms.szs"), "The path names an archive but no file inside it.");
}

MODS_TEST(paths_keys)
{
	EntryKey k = entry_key("files/user/Abe/vs/Stages.TXT");
	MODS_CHECK(k.ok && !k.archive);
	MODS_CHECK_EQ(k.key, "/user/abe/vs/stages.txt");
	k = entry_key("archives/enemy/parm/enemyParms.szs/Kochappy/enemyparm.txt");
	MODS_CHECK(k.ok && k.archive);
	MODS_CHECK_EQ(k.key, "/enemy/parm/enemyparms.szs:kochappy/enemyparm.txt");
	MODS_CHECK_EQ(k.archivePath, "/enemy/parm/enemyparms.szs");
	MODS_CHECK_EQ(k.innerPath, "kochappy/enemyparm.txt");
	// The split is at the first .szs/.arc component.
	k = entry_key("archives/user/Abe/Pellet/us/pelletlist_us.szs/numberpellet_config.txt");
	MODS_CHECK(k.ok);
	MODS_CHECK_EQ(k.key, "/user/abe/pellet/us/pelletlist_us.szs:numberpellet_config.txt");
	k = entry_key("archives/a/outer.ARC/inner.szs/x.bin");
	MODS_CHECK(k.ok);
	MODS_CHECK_EQ(k.archivePath, "/a/outer.arc");
	MODS_CHECK_EQ(k.innerPath, "inner.szs/x.bin");
	// A whole archive as a loose file is a plain key.
	k = entry_key("files/enemy/parm/enemyParms.szs");
	MODS_CHECK(k.ok && !k.archive);
	MODS_CHECK_EQ(k.key, "/enemy/parm/enemyparms.szs");
	// Text helpers.
	MODS_CHECK_EQ(strip_controls("a\tb\nc\x7f" "d\xc2\x85" "e\xc3\xa9"), "abcde\xc3\xa9");
	MODS_CHECK_EQ(utf8_length("caf\xc3\xa9"), 4u);
	MODS_CHECK(valid_id("mppm-test-1"));
	MODS_CHECK(!valid_id("Mppm"));
	MODS_CHECK(!valid_id(""));
	MODS_CHECK(!valid_id(std::string(65, 'a')));
	MODS_CHECK(!valid_id("a_b"));
}

MODS_TEST(manifest_good_and_optional)
{
	Manifest m = mf(R"({"id":"my-mod","version":"1.2.3-beta.1+build.5","name":"My\u0007 Mod","author":"me",
		"description":"Line one\nline two","affects":"cosmetic","mppm":">=0.1","files":["files/a.txt","archives/x.szs/y.txt"]})");
	MODS_CHECK_EQ(errors(m.issues), 0);
	MODS_CHECK_EQ(m.issues.size(), 0u);
	MODS_CHECK_EQ(m.id, "my-mod");
	MODS_CHECK_EQ(m.version, "1.2.3-beta.1+build.5");
	MODS_CHECK_EQ(m.name, "My Mod");
	MODS_CHECK_EQ(m.description, "Line oneline two");
	MODS_CHECK(m.cosmetic);
	MODS_CHECK_EQ(m.minMppm, ">=0.1");
	MODS_CHECK_EQ(m.files.size(), 2u);
	m = mf(kGood);
	MODS_CHECK_EQ(m.issues.size(), 0u);
	MODS_CHECK_EQ(m.name, "my-mod"); // the id when missing
	MODS_CHECK(!m.cosmetic);
}

MODS_TEST(manifest_errors)
{
	MODS_CHECK(has_issue(mf("{\"id\": ").issues, Severity::Error, "not valid JSON"));
	MODS_CHECK(has_issue(mf("").issues, Severity::Error, "not valid JSON"));
	MODS_CHECK(has_issue(mf("[1,2]").issues, Severity::Error, "must hold a JSON object"));
	MODS_CHECK(has_issue(mf(std::string(10000, '[')).issues, Severity::Error, "deeper than 16"));
	MODS_CHECK(has_issue(mf(std::string(10000, '{')).issues, Severity::Error, "deeper than 16"));
	// Brackets inside a string do not count.
	MODS_CHECK_EQ(json_depth(R"({"a":"[[[[[[[[[[[[[[[[[[[[[[[[","b":"\"{{{{"})"), 1);
	MODS_CHECK(has_issue(mf(std::string(70 * 1024, ' ') + kGood).issues, Severity::Error, "larger than 64 KiB"));
	MODS_CHECK(has_issue(mf(R"({"version":"1.0.0","files":["files/a"]})").issues, Severity::Error, "\"id\" is missing"));
	MODS_CHECK(has_issue(mf(R"({"id":5,"version":"1.0.0","files":["files/a"]})").issues, Severity::Error, "\"id\" must be a string"));
	MODS_CHECK(has_issue(mf(R"({"id":"Other","version":"1.0.0","files":["files/a"]})").issues, Severity::Error, "1 to 64 characters"));
	MODS_CHECK(has_issue(mf(R"({"id":"other","version":"1.0.0","files":["files/a"]})").issues, Severity::Error, "equal the folder name"));
	for (const char* v : {"1.0", "01.0.0", "1.0.0-", "1.0.0+", "a.b.c", "1.0.0.0", "1.0.0-be ta", "1.0.0-abcdefghijklmnopqrstuvwxyz0123"}) {
		const std::string j = std::string(R"({"id":"my-mod","files":["files/a"],"version":")") + v + "\"}";
		MODS_CHECK(has_issue(mf(j).issues, Severity::Error, "\"version\" must be"));
	}
	MODS_CHECK(has_issue(mf(R"({"id":"my-mod","files":["files/a"],"version":1})").issues, Severity::Error, "\"version\" must be"));
	MODS_CHECK(has_issue(mf(R"({"id":"my-mod","files":["files/a"]})").issues, Severity::Error, "\"version\" is missing"));
	MODS_CHECK(has_issue(mf(R"({"id":"my-mod","version":"1.0.0"})").issues, Severity::Error, "\"files\" is missing"));
	MODS_CHECK(has_issue(mf(R"({"id":"my-mod","version":"1.0.0","files":"files/a"})").issues, Severity::Error, "must be an array"));
	MODS_CHECK(has_issue(mf(R"({"id":"my-mod","version":"1.0.0","files":[]})").issues, Severity::Error, "1 to 4096"));
	MODS_CHECK(has_issue(mf(R"({"id":"my-mod","version":"1.0.0","files":["files/a",3]})").issues, Severity::Error, "only strings"));
	MODS_CHECK(has_issue(mf(R"({"id":"my-mod","version":"1.0.0","files":["files/a"],"name":7})").issues, Severity::Error, "\"name\" must be a string"));
	MODS_CHECK(has_issue(mf(R"({"id":"my-mod","version":"1.0.0","files":["files/a"],"name":""})").issues, Severity::Error, "\"name\" must be 1 to 64"));
	MODS_CHECK(has_issue(mf(std::string(R"({"id":"my-mod","version":"1.0.0","files":["files/a"],"name":")") + std::string(65, 'n') + "\"}").issues,
	                     Severity::Error, "\"name\" must be 1 to 64"));
	MODS_CHECK(has_issue(mf(std::string(R"({"id":"my-mod","version":"1.0.0","files":["files/a"],"author":")") + std::string(65, 'n') + "\"}").issues,
	                     Severity::Error, "\"author\" is longer than 64"));
	MODS_CHECK(has_issue(mf(std::string(R"({"id":"my-mod","version":"1.0.0","files":["files/a"],"description":")") + std::string(1025, 'n') + "\"}").issues,
	                     Severity::Error, "\"description\" is longer than 1024"));
	MODS_CHECK(has_issue(mf(R"({"id":"my-mod","version":"1.0.0","files":["files/a"],"affects":"both"})").issues, Severity::Error, "\"affects\""));
	// mppm: newer than this build, malformed.
	MODS_CHECK(has_issue(mf(R"({"id":"my-mod","version":"1.0.0","files":["files/a"],"mppm":">=99.0"})").issues, Severity::Error,
	                     "Needs Moonsprout 99.0 or newer."));
	MODS_CHECK(has_issue(mf(R"({"id":"my-mod","version":"1.0.0","files":["files/a"],"mppm":">=0.1.1"})").issues, Severity::Error,
	                     "Needs Moonsprout 0.1.1 or newer."));
	MODS_CHECK_EQ(errors(mf(R"({"id":"my-mod","version":"1.0.0","files":["files/a"],"mppm":">=0.1.0"})").issues), 0);
	MODS_CHECK_EQ(errors(mf(R"({"id":"my-mod","version":"1.0.0","files":["files/a"],"mppm":">=0"})").issues), 0);
	for (const char* v : {"0.1", ">0.1", ">=", ">=0.1.2.3", ">=a", ">=0..1", ">=1234567890"}) {
		const std::string j = std::string(R"({"id":"my-mod","version":"1.0.0","files":["files/a"],"mppm":")") + v + "\"}";
		MODS_CHECK(has_issue(mf(j).issues, Severity::Error, "\"mppm\" must look like"));
	}
	MODS_CHECK(has_issue(mf(R"({"id":"my-mod","version":"1.0.0","files":["files/a"],"mppm":1})").issues, Severity::Error, "\"mppm\" must be a string"));
	// Unknown key: a warning only.
	Manifest m = mf(R"({"id":"my-mod","version":"1.0.0","files":["files/a"],"homepage":"x"})");
	MODS_CHECK_EQ(errors(m.issues), 0);
	MODS_CHECK(has_issue(m.issues, Severity::Warning, "Unknown key \"homepage\" ignored."));
	// Invalid UTF-8 is a parse error, not a crash.
	MODS_CHECK(has_issue(mf("{\"id\":\"my-mod\",\"name\":\"\xff\xfe\"}").issues, Severity::Error, "not valid JSON"));
	// Version prefix.
	const Version3 v = numeric_prefix("0.1.0-dev+g1234");
	MODS_CHECK(v.major == 0 && v.minor == 1 && v.patch == 0);
	const Version3 w = numeric_prefix("2.10");
	MODS_CHECK(w.major == 2 && w.minor == 10 && w.patch == 0);
}

MODS_TEST(load_order_parse)
{
	LoadOrder o = parse_load_order("\xEF\xBB\xBF# header\n\n  alpha  \r\n-beta\n- gamma\n#off\nalpha\nBad Line\n-\ndelta");
	MODS_CHECK_EQ(o.entries.size(), 4u);
	MODS_CHECK(o.entries[0].id == "alpha" && o.entries[0].enabled);
	MODS_CHECK(o.entries[1].id == "beta" && !o.entries[1].enabled);
	MODS_CHECK(o.entries[2].id == "gamma" && !o.entries[2].enabled);
	MODS_CHECK(o.entries[3].id == "delta" && o.entries[3].enabled);
	MODS_CHECK(has_issue(o.issues, Severity::Warning, "Line 7 of load_order.txt repeats alpha"));
	MODS_CHECK(has_issue(o.issues, Severity::Warning, "Line 8 of load_order.txt is not a mod id."));
	MODS_CHECK(has_issue(o.issues, Severity::Warning, "Line 9 of load_order.txt is not a mod id."));
	MODS_CHECK_EQ(o.issues.size(), 3u);
	// Duplicate with a different state: the first counts.
	o = parse_load_order("-a\na\n");
	MODS_CHECK(o.entries.size() == 1 && !o.entries[0].enabled);
	// Too big, too many lines.
	o = parse_load_order(std::string(64 * 1024 + 1, '#'));
	MODS_CHECK(o.entries.empty() && has_issue(o.issues, Severity::Error, "larger than 64 KiB"));
	std::string many;
	for (int i = 0; i < 1100; i++) many += "m" + std::to_string(i) + "\n";
	o = parse_load_order(many);
	MODS_CHECK_EQ(o.entries.size(), 1024u);
	MODS_CHECK(has_issue(o.issues, Severity::Warning, "more than 1024 lines"));
	// Round trip of what the launcher writes.
	const std::string text = format_load_order({{"alpha", true}, {"beta", false}});
	MODS_CHECK_EQ(text, "# Moonsprout mod load order: top = lowest priority, bottom = highest. \"-\" turns a mod off.\nalpha\n-beta\n");
	o = parse_load_order(text);
	MODS_CHECK(o.entries.size() == 2 && o.issues.empty());
}

MODS_TEST(validators_stage_lists)
{
	const auto run = [](const std::string& key, const std::string& text) {
		std::vector<Issue> issues;
		validate_file(key, "files" + key, reinterpret_cast<const uint8_t*>(text.data()), text.size(), issues);
		return issues;
	};
	const std::string vs = "/user/abe/vs/stages.txt";
	const std::string ch = "/user/matoba/challenge/stages.txt";
	MODS_CHECK(run(vs, stage_list(10, 10)).empty());
	MODS_CHECK(run(vs, stage_list(1, 1)).empty());
	MODS_CHECK(has_issue(run(vs, stage_list(11, 11)), Severity::Error, "has 11 stages; the stage select shows at most 10."));
	MODS_CHECK(has_issue(run(vs, stage_list(0, 0)), Severity::Error, "needs at least 1"));
	MODS_CHECK(has_issue(run(vs, stage_list(10, 9)), Severity::Error, "says 10 stages but has 9."));
	MODS_CHECK(has_issue(run(vs, "# nothing\n{\n}\n"), Severity::Error, "does not start with its number of stages"));
	MODS_CHECK(run(ch, stage_list(30, 30)).empty());
	MODS_CHECK(run("/user/matoba/challenge/kfes-stages.txt", stage_list(31, 31)).empty());
	MODS_CHECK(has_issue(run(ch, stage_list(29, 29)), Severity::Error, "has 29 stages; it needs at least 30."));
	MODS_CHECK(has_issue(run("/user/matoba/challenge/kfes-stages.txt", stage_list(29, 29)), Severity::Error, "at least 30"));
	// Nested blocks count once; braces inside tokens ({f002}) are not blocks.
	MODS_CHECK(run(vs, "1\n{\n {f002} 4 30\n {\n }\n}\n").empty());
	// Loose archives need Yaz0 / RARC; inner files are not checked for it.
	MODS_CHECK(has_issue(run("/enemy/parm/enemyparms.szs", "hello"), Severity::Error, "not a Yaz0 or RARC archive"));
	MODS_CHECK(run("/a/b.szs", std::string("Yaz0\0\0\0\0", 8)).empty());
	MODS_CHECK(run("/a/b.arc", "RARCxxxx").empty());
	MODS_CHECK(run("/a/b.szs:c.arc", "hello").empty());
	// Text warnings.
	std::vector<Issue> w = run("/user/x.txt", std::string("a\0b", 3));
	MODS_CHECK(has_issue(w, Severity::Warning, "NUL byte") && errors(w) == 0);
	w = run("/user/x.ini", "{\n{\n}\n");
	MODS_CHECK(has_issue(w, Severity::Warning, "do not balance") && errors(w) == 0);
	MODS_CHECK(has_issue(run("/user/x.txt", "}\n{\n"), Severity::Warning, "do not balance"));
	MODS_CHECK(run("/user/x.txt", "{ # } \n}\n").empty()); // the } in a comment does not count
	MODS_CHECK(run("/a.szs:x/enemyparm.txt", "{\n").size() == 1);
}

MODS_TEST(conflicts_last_wins)
{
	const std::vector<Conflict> c = find_conflicts({
	    {"a", {"/k1", "/k2"}},
	    {"b", {"/k1", "/k3"}},
	    {"c", {"/k1", "/k2", "/k4"}},
	});
	MODS_CHECK_EQ(c.size(), 2u);
	MODS_CHECK_EQ(c[0].key, "/k1");
	MODS_CHECK_EQ(c[0].winner, "c");
	MODS_CHECK(c[0].losers == (std::vector<std::string>{"b", "a"}));
	MODS_CHECK_EQ(c[1].key, "/k2");
	MODS_CHECK_EQ(c[1].winner, "c");
	MODS_CHECK(c[1].losers == (std::vector<std::string>{"a"}));
	MODS_CHECK(find_conflicts({{"a", {"/x"}}, {"b", {"/y"}}}).empty());
}

MODS_TEST(set_hash_vectors)
{
	const uint64_t build = 0x0123456789abcdefull;
	const auto file = [](const char* key, const std::string& bytes) {
		return SetFile{key, bytes.size(), Sha256::of(bytes.data(), bytes.size())};
	};
	const SetMod alpha{"alpha", "1.0.0", {file("/user/abe/vs/stages.txt", "10\n")}};
	// Files given out of order: the serialization sorts them.
	const SetMod beta{"beta", "0.2.1",
	                  {file("/user/mukki/mapunits/caveinfo/test.txt", "hello\n"),
	                   file("/enemy/parm/enemyparms.szs:chappy/enemyparm.txt", "x")}};
	const std::vector<uint8_t> s = serialize_set({alpha, beta});
	MODS_CHECK_EQ(s.size(), 283u);
	MODS_CHECK_EQ(hex_of(content_hash_of(build, s)), "699dce5678b94e4d309afa06d1e0de3c8c213ae8b1788d05cfc8ff3a07fd6ac3");
	MODS_CHECK_EQ(hex_of(mods_hash_of(s, 2)), "d54c604f3bb5fb1d3f40e5b0cd870aa43c26f9c1a0ecaee2b490e16eaf7c531c");
	MODS_CHECK_EQ(hex_of(content_hash_of(build, {})), "553c9181fbcecab7b9b7d1ce992639878b12c35427e39fd5b92ddc2ee8d1c001");
	const std::vector<uint8_t> r = serialize_set({beta, alpha});
	MODS_CHECK_EQ(r.size(), 283u);
	MODS_CHECK_EQ(hex_of(content_hash_of(build, r)), "fe7fbbbee89acbfb88b3f51ec383c06bf898775289949e123583ccdb5ac48e53");
	MODS_CHECK_EQ(hex_of(mods_hash_of(r, 2)), "9fd19ce6bdc8c5be6150849c3323c21900ef97535807c99aa92916ac003748ff");
	MODS_CHECK(mods_hash_of({}, 0) == Hash{});
	MODS_CHECK_EQ(hex_of(Hash{}, 8), "00000000");
}

MODS_TEST(scan_temp_folder)
{
	const char* tmpEnv = std::getenv("TMPDIR");
	const fs::path base = fs::path(tmpEnv != nullptr && tmpEnv[0] != '\0' ? tmpEnv : "/var/tmp") /
	                      ("p2mods_selftest_" + std::to_string((long)getpid()));
	std::error_code ec;
	fs::remove_all(base, ec);
	const fs::path mods = base / "mods";
	fs::create_directories(mods);

	// Stock-like: the folder exists, no load_order.txt.
	{
		FullScan f = scan_dir(mods, "0.1.0", {});
		MODS_CHECK(f.scan.exists && f.scan.mods.empty() && f.scan.enabledValid == 0 && f.scan.modsHash == Hash{});
		MODS_CHECK(f.scan.setBytes.empty());
	}

	// good: a VS list (10 synthetic stages) and an archive file; also reached through a symlinked folder.
	write(mods / "good/mod.json",
	      R"({"id":"good","version":"1.0.0","name":"Good","files":["files/user/Abe/vs/stages.txt","archives/enemy/parm/enemyParms.szs/kochappy/enemyparm.txt"]})");
	write(mods / "good/files/user/Abe/vs/stages.txt", stage_list(10, 10));
	write(mods / "good/archives/enemy/parm/enemyParms.szs/kochappy/enemyparm.txt", "{\n}\n");
	write(mods / "good/files/unlisted.txt", "ignored");
	// linked: a mod folder that is a symlink (allowed), overriding good's stages.txt.
	write(base / "elsewhere/linked/mod.json", R"({"id":"linked","version":"0.0.1","files":["files/user/abe/vs/stages.txt"]})");
	write(base / "elsewhere/linked/files/user/abe/vs/stages.txt", stage_list(1, 1));
	fs::create_directory_symlink(base / "elsewhere/linked", mods / "linked");
	// symlinked file inside a mod: refused.
	write(mods / "symfile/mod.json", R"({"id":"symfile","version":"1.0.0","files":["files/a.txt"]})");
	fs::create_directories(mods / "symfile/files");
	write(base / "secret.txt", "outside");
	fs::create_symlink(base / "secret.txt", mods / "symfile/files/a.txt");
	// symlinked folder inside a mod: refused.
	write(mods / "symdir/mod.json", R"({"id":"symdir","version":"1.0.0","files":["files/sub/a.txt"]})");
	fs::create_directories(mods / "symdir/files");
	write(base / "outdir/a.txt", "outside");
	fs::create_directory_symlink(base / "outdir", mods / "symdir/files/sub");
	// missing listed file.
	write(mods / "missing/mod.json", R"({"id":"missing","version":"1.0.0","files":["files/here.txt","files/gone.txt"]})");
	write(mods / "missing/files/here.txt", "x");
	// sparse 70 MiB file: refused by size, never read.
	write(mods / "huge/mod.json", R"({"id":"huge","version":"1.0.0","files":["files/big.bin"]})");
	fs::create_directories(mods / "huge/files");
	{
		std::ofstream(mods / "huge/files/big.bin").close();
		fs::resize_file(mods / "huge/files/big.bin", 70ull << 20);
	}
	// bad VS list (11 stages), wrong id, broken JSON, no manifest, hidden folder, a plain file.
	write(mods / "eleven/mod.json", R"({"id":"eleven","version":"1.0.0","files":["files/user/abe/vs/stages.txt"]})");
	write(mods / "eleven/files/user/abe/vs/stages.txt", stage_list(11, 11));
	write(mods / "wrongid/mod.json", R"({"id":"other","version":"1.0.0","files":["files/a.txt"]})");
	write(mods / "wrongid/files/a.txt", "x");
	write(mods / "broken/mod.json", "{\"id\": \"broken\", ");
	fs::create_directories(mods / "empty");
	write(mods / ".cache/mod.json", "{}");
	write(mods / "README.txt", "not a mod");
	// new: not listed.
	write(mods / "newmod/mod.json", R"({"id":"newmod","version":"2.0.0","files":["files/n.txt"]})");
	write(mods / "newmod/files/n.txt", "n");
	write(mods / "load_order.txt",
	      "# test\ngood\nlinked\nsymfile\nsymdir\nmissing\nhuge\neleven\nwrongid\nbroken\n-empty\nghost\nbad line\n");

	FullScan f = scan_dir(mods, "0.1.0", {});
	const Scan& s = f.scan;
	const auto find = [&](const std::string& id) -> const ModInfo* {
		for (const ModInfo& m : s.mods) {
			if (m.id == id) return &m;
		}
		return nullptr;
	};
	// Order: listed first in file order (ghost included), then unlisted by name.
	std::vector<std::string> ids;
	for (const ModInfo& m : s.mods) ids.push_back(m.id);
	MODS_CHECK(ids == (std::vector<std::string>{"good", "linked", "symfile", "symdir", "missing", "huge", "eleven", "wrongid",
	                                            "broken", "empty", "ghost", "newmod"}));
	MODS_CHECK(has_issue(s.issues, Severity::Warning, "Line 13 of load_order.txt is not a mod id."));

	const ModInfo* good = find("good");
	MODS_CHECK(good && good->valid && good->enabled && good->listed && good->present);
	MODS_CHECK(good && good->name == "Good" && good->fileCount == 2);
	MODS_CHECK(good && good->keys == (std::vector<std::string>{"/enemy/parm/enemyparms.szs:kochappy/enemyparm.txt",
	                                                          "/user/abe/vs/stages.txt"}));
	const ModInfo* linked = find("linked");
	MODS_CHECK(linked && linked->valid);
	const ModInfo* symfile = find("symfile");
	MODS_CHECK(symfile && !symfile->valid && has_issue(symfile->issues, Severity::Error, "symbolic link"));
	const ModInfo* symdir = find("symdir");
	MODS_CHECK(symdir && !symdir->valid && has_issue(symdir->issues, Severity::Error, "symbolic link"));
	const ModInfo* missing = find("missing");
	MODS_CHECK(missing && !missing->valid && has_issue(missing->issues, Severity::Error, "The file is missing."));
	MODS_CHECK(missing && missing->issues.size() == 1 && missing->issues[0].path == "files/gone.txt");
	const ModInfo* huge = find("huge");
	MODS_CHECK(huge && !huge->valid && has_issue(huge->issues, Severity::Error, "larger than 64 MiB"));
	const ModInfo* eleven = find("eleven");
	MODS_CHECK(eleven && !eleven->valid && has_issue(eleven->issues, Severity::Error, "11 stages"));
	const ModInfo* wrongid = find("wrongid");
	MODS_CHECK(wrongid && !wrongid->valid && has_issue(wrongid->issues, Severity::Error, "equal the folder name"));
	const ModInfo* broken = find("broken");
	MODS_CHECK(broken && !broken->valid && has_issue(broken->issues, Severity::Error, "not valid JSON"));
	const ModInfo* empty = find("empty");
	MODS_CHECK(empty && !empty->enabled && !empty->valid && has_issue(empty->issues, Severity::Error, "no mod.json"));
	const ModInfo* ghost = find("ghost");
	MODS_CHECK(ghost && !ghost->present && !ghost->valid && ghost->enabled);
	MODS_CHECK(ghost && has_issue(ghost->issues, Severity::Warning, "folder is missing"));
	const ModInfo* newmod = find("newmod");
	MODS_CHECK(newmod && !newmod->listed && !newmod->enabled && newmod->valid && newmod->version == "2.0.0");

	MODS_CHECK_EQ(s.enabledValid, 2);
	MODS_CHECK_EQ(s.conflicts.size(), 1u);
	if (!s.conflicts.empty()) {
		MODS_CHECK_EQ(s.conflicts[0].key, "/user/abe/vs/stages.txt");
		MODS_CHECK_EQ(s.conflicts[0].winner, "linked");
		MODS_CHECK(s.conflicts[0].losers == (std::vector<std::string>{"good"}));
	}
	// The hash follows S of good then linked, every listed file included.
	const std::string vs10 = stage_list(10, 10), vs1 = stage_list(1, 1), parm = "{\n}\n";
	const std::vector<uint8_t> expect = serialize_set({
	    {"good", "1.0.0",
	     {{"/user/abe/vs/stages.txt", vs10.size(), Sha256::of(vs10.data(), vs10.size())},
	      {"/enemy/parm/enemyparms.szs:kochappy/enemyparm.txt", parm.size(), Sha256::of(parm.data(), parm.size())}}},
	    {"linked", "0.0.1", {{"/user/abe/vs/stages.txt", vs1.size(), Sha256::of(vs1.data(), vs1.size())}}},
	});
	MODS_CHECK(s.setBytes == expect);
	MODS_CHECK(s.modsHash == mods_hash_of(expect, 2));

	// Mount view (readAll = false): the new mod's file is not read, the result is the same set.
	FullScan g = scan_dir(mods, "0.1.0", {false, false});
	MODS_CHECK(g.scan.modsHash == s.modsHash);
	// Off: an empty scan.
	FullScan off = scan_dir(mods, "0.1.0", {true, true});
	MODS_CHECK(off.scan.off && off.scan.mods.empty() && off.scan.dir.empty());
	// Missing folder.
	FullScan none = scan_dir(base / "nope", "0.1.0", {});
	MODS_CHECK(!none.scan.exists && none.scan.mods.empty());

	fs::remove_all(base, ec);
	MODS_CHECK(!ec);
}

// Fix round 3, item 6: a whole-archive replacement (loose "<arc>") and inner files ("<arc>:<inner>")
// follow the load order: the later one wins, and the pair is a conflict either way.
MODS_TEST(whole_archive_vs_inner)
{
	const std::string arc = "/enemy/parm/enemyparms.szs";
	const std::string k1 = arc + ":kochappy/enemyparm.txt", k2 = arc + ":egg/enemyparm.txt";
	MODS_CHECK_EQ(archive_of_key(k1), arc);
	MODS_CHECK_EQ(archive_of_key(arc), "");

	// inner (b) before whole (w): b's inner file drops, w overrides b.
	{
		const std::vector<KeyedMod> m = {{"b", {k1}}, {"w", {arc}}};
		const auto win = pick_winners(m);
		MODS_CHECK(win.count(arc) == 1 && win.at(arc) == 1);
		MODS_CHECK(win.count(k1) == 0);
		const auto c = find_conflicts(m);
		MODS_CHECK_EQ(c.size(), 1u);
		if (c.size() == 1) {
			MODS_CHECK_EQ(c[0].key, k1);
			MODS_CHECK_EQ(c[0].winner, "w");
			MODS_CHECK(c[0].losers == (std::vector<std::string>{"b"}));
			MODS_CHECK_EQ(c[0].archiveMod, "w");
		}
	}
	// whole (w) before inner (b): b's inner file goes on top, b overrides w.
	{
		const std::vector<KeyedMod> m = {{"w", {arc}}, {"b", {k1}}};
		const auto win = pick_winners(m);
		MODS_CHECK(win.count(k1) == 1 && win.at(k1) == 1 && win.at(arc) == 0);
		const auto c = find_conflicts(m);
		MODS_CHECK_EQ(c.size(), 1u);
		if (c.size() == 1) {
			MODS_CHECK_EQ(c[0].winner, "b");
			MODS_CHECK(c[0].losers == (std::vector<std::string>{"w"}));
			MODS_CHECK_EQ(c[0].archiveMod, "w");
		}
	}
	// a, then w, then c: c's k1 wins over w's archive and a's k1; a's k2 drops under w.
	{
		const std::vector<KeyedMod> m = {{"a", {k1, k2}}, {"w", {arc}}, {"c", {k1}}};
		const auto win = pick_winners(m);
		MODS_CHECK(win.count(k1) == 1 && win.at(k1) == 2);
		MODS_CHECK(win.count(k2) == 0);
		const auto c = find_conflicts(m); // sorted by key: k2 (egg) before k1 (kochappy)
		MODS_CHECK_EQ(c.size(), 2u);
		if (c.size() == 2) {
			MODS_CHECK_EQ(c[0].key, k2);
			MODS_CHECK_EQ(c[0].winner, "w");
			MODS_CHECK(c[0].losers == (std::vector<std::string>{"a"}));
			MODS_CHECK_EQ(c[1].key, k1);
			MODS_CHECK_EQ(c[1].winner, "c");
			MODS_CHECK(c[1].losers == (std::vector<std::string>{"w", "a"}));
		}
	}
	// One mod with both: its inner file goes on top of its own archive; no conflict.
	{
		const std::vector<KeyedMod> m = {{"w", {arc, k1}}};
		MODS_CHECK(pick_winners(m).count(k1) == 1);
		MODS_CHECK(find_conflicts(m).empty());
	}
	// Two whole-archive replacements: only the last one's archive counts.
	{
		const std::vector<KeyedMod> m = {{"w1", {arc}}, {"b", {k1}}, {"w2", {arc}}};
		MODS_CHECK(pick_winners(m).count(k1) == 0);
		const auto c = find_conflicts(m);
		MODS_CHECK_EQ(c.size(), 2u); // arc (w2 over w1) and k1 (w2 over b)
		if (c.size() == 2) {
			MODS_CHECK_EQ(c[0].key, arc);
			MODS_CHECK(c[0].archiveMod.empty());
			MODS_CHECK_EQ(c[1].winner, "w2");
			MODS_CHECK(c[1].losers == (std::vector<std::string>{"b"}));
		}
	}
	// Other archives and loose keys are untouched.
	MODS_CHECK(find_conflicts({{"w", {arc}}, {"b", {"/other.szs:x.txt"}}}).empty());
	MODS_CHECK_EQ(pick_winners({{"w", {arc}}, {"b", {"/other.szs:x.txt"}}}).size(), 2u);
}

namespace {
fs::path temp_base(const char* tag)
{
	const char* tmpEnv = std::getenv("TMPDIR");
	return fs::path(tmpEnv != nullptr && tmpEnv[0] != '\0' ? tmpEnv : "/var/tmp") /
	       (std::string("p2mods_selftest_") + tag + "_" + std::to_string((long)getpid()));
}
} // namespace

// Fix round 3, item 2: a folder whose name is not an id and an off line with no folder are reported
// (off_notices: what the mount logs for mods that are not on).
MODS_TEST(off_notices_folder_and_gone)
{
	const fs::path base = temp_base("notices");
	std::error_code ec;
	fs::remove_all(base, ec);
	const fs::path mods = base / "mods";
	write(mods / "Bad Folder Name/mod.json", R"({"id":"bad-folder-name","version":"1.0.0","files":["files/a.txt"]})");
	write(mods / "Bad Folder Name/files/a.txt", "a");
	write(mods / "ok/mod.json", R"({"id":"ok","version":"1.0.0","files":["files/a.txt"]})");
	write(mods / "ok/files/a.txt", "a");
	write(mods / "off-broken/mod.json", "{");
	write(mods / "load_order.txt", "ok\n-gone\nghost\n-off-broken\n");

	const Scan s = scan_dir(mods, "0.1.0", {}).scan;
	const ModInfo* bad = nullptr;
	for (const ModInfo& m : s.mods) {
		if (m.id == "Bad Folder Name") bad = &m;
	}
	MODS_CHECK(bad != nullptr && !bad->listed && !bad->valid);
	MODS_CHECK(bad && has_issue(bad->issues, Severity::Warning, "not a mod id"));
	const auto n = off_notices(s);
	MODS_CHECK_EQ(n.size(), 2u); // not ghost (on: logged with the mods that are on), not off-broken
	if (n.size() == 2) {
		MODS_CHECK_EQ(n[0].first, "gone");
		MODS_CHECK(n[0].second.severity == Severity::Warning &&
		           n[0].second.message == "load_order.txt lists this mod, but its folder is missing.");
		MODS_CHECK_EQ(n[1].first, "Bad Folder Name");
		MODS_CHECK(n[1].second.severity == Severity::Warning && n[1].second.message == kBadFolderName);
	}
	MODS_CHECK_EQ(s.enabledValid, 1);
	fs::remove_all(base, ec);
}

// Fix round 3, item 5: the restart rule compares the folder's mods hash now with the one the boot
// read before the mount dropped any mod (Mounted::folderHash). A disc-dependent drop alone is no
// change; editing or updating any mod, the dropped one included, is.
MODS_TEST(restart_rule_folder_hash)
{
	const fs::path base = temp_base("restart");
	std::error_code ec;
	fs::remove_all(base, ec);
	const fs::path mods = base / "mods";
	write(mods / "a/mod.json", R"({"id":"a","version":"1.0.0","files":["files/a.txt"]})");
	write(mods / "a/files/a.txt", "a");
	write(mods / "d/mod.json", R"({"id":"d","version":"1.0.0","files":["archives/x.szs/f.txt"]})");
	write(mods / "d/archives/x.szs/f.txt", "f");
	write(mods / "load_order.txt", "a\nd\n");

	// The boot: scan, remember the folder hash, then the mount drops d (archive not on the disc).
	FullScan boot = scan_dir(mods, "0.1.0", {false, false});
	const Hash folderHash = boot.scan.modsHash;
	boot.scan.mods[1].valid = false;
	finish_set(boot);
	MODS_CHECK(boot.scan.modsHash != folderHash); // the mounted set is a alone

	const auto pending = [&] { return scan_dir(mods, "0.1.0", {}).scan.modsHash != folderHash; };
	MODS_CHECK(!pending()); // nothing changed: the drop alone is not pending
	write(mods / "d/archives/x.szs/f.txt", "g"); // the dropped mod is edited
	MODS_CHECK(pending());
	write(mods / "d/archives/x.szs/f.txt", "f");
	MODS_CHECK(!pending());
	write(mods / "a/mod.json", R"({"id":"a","version":"1.0.1","files":["files/a.txt"]})"); // updated
	MODS_CHECK(pending());
	write(mods / "a/mod.json", R"({"id":"a","version":"1.0.0","files":["files/a.txt"]})");
	write(mods / "load_order.txt", "d\na\n"); // same mods, other order
	MODS_CHECK(pending());
	write(mods / "load_order.txt", "a\n-d\n"); // the dropped mod turned off
	MODS_CHECK(pending());
	write(mods / "load_order.txt", "a\nd\n");
	MODS_CHECK(!pending());
	fs::remove_all(base, ec);
}
