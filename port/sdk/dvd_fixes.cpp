// DVD path handling the game relies on (see p2_sdk_compat.h). Calls Aurora's real functions.
#include <dolphin/dvd.h>

namespace {
// "a/b//c/d" -> "/c/d": a component that starts with '/' resets the lookup to the root.
const char* sdkPath(const char* path)
{
	if (path == nullptr) {
		return path;
	}
	const char* result = path;
	for (const char* p = path; p[0] != '\0'; p++) {
		if (p[0] == '/' && p[1] == '/') {
			result = p + 1;
		}
	}
	return result;
}
} // namespace

extern "C" {
s32 p2_DVDConvertPathToEntrynum(const char* path) { return DVDConvertPathToEntrynum(sdkPath(path)); }
BOOL p2_DVDOpen(const char* path, DVDFileInfo* info) { return DVDOpen(sdkPath(path), info); }
BOOL p2_DVDOpenDir(const char* path, DVDDir* dir) { return DVDOpenDir(sdkPath(path), dir); }
BOOL p2_DVDChangeDir(const char* path) { return DVDChangeDir(sdkPath(path)); }
}
