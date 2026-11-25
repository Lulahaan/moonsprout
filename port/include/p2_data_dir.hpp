#pragma once
// The folder that holds settings, saves, mods, input bindings and online logs.
//
// Normally that is SDL's per-user folder, SDL_GetPrefPath(nullptr, "mppm") (the name is
// mppm::branding::kDataDirName). A portable install, marked by a file named "portable.txt" next to
// the executable (the Windows portable zip ships one), keeps everything in "<exe folder>/data/"
// instead, so it can live on a USB stick.
//
// The result is UTF-8, ends with a path separator, and is empty when no folder could be made.
// Computed once per process.
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

#include <string>

namespace mppm {

inline const std::string& data_dir_utf8() {
    static const std::string sDir = [] {
        std::string dir;
        if (const char* base = SDL_GetBasePath(); base != nullptr && base[0] != '\0') {
            const std::string baseDir = base; // ends with the platform's separator
            SDL_PathInfo info{};
            if (SDL_GetPathInfo((baseDir + "portable.txt").c_str(), &info) && info.type == SDL_PATHTYPE_FILE) {
                dir = baseDir + "data";
                if (SDL_CreateDirectory(dir.c_str())) {
                    dir += baseDir.back();
                    return dir;
                }
                dir.clear(); // read-only folder: fall back to the per-user folder
            }
        }
        if (char* pref = SDL_GetPrefPath(nullptr, "mppm")) {
            dir = pref;
            SDL_free(pref);
        }
        return dir;
    }();
    return sDir;
}

// True when data_dir_utf8() is the portable "<exe folder>/data/" folder.
inline bool data_dir_is_portable() {
    const char* base = SDL_GetBasePath();
    const std::string& dir = data_dir_utf8();
    return base != nullptr && base[0] != '\0' && dir.rfind(base, 0) == 0;
}

} // namespace mppm
