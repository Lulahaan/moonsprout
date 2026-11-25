// Restarting the app (docs/launcher/SETTINGS.md 11): re-executes the binary with the original
// arguments, `--launcher` or `--play` replacing any earlier one, after the same teardown as
// AURORA_EXIT in port/main.cpp.
//
// The original arguments are read back from the operating system (/proc/self/cmdline,
// _NSGetArgv, GetCommandLineW) rather than kept by launcher::preinit, so a disc given on the
// command line, --cvar overrides and Aurora's own options all come back unchanged.
//
// aurora_shutdown() is deliberately not called: request_restart runs from a UI callback, inside
// RmlUi's event dispatch and an open frame, and Aurora's shutdown would destroy that UI under its
// own call stack. The new process image releases the window, the GPU device and the audio device
// with the old one (every descriptor is marked close-on-exec first). What must reach the disk is
// written before: the settings file is flushed here, memory card writes and controller mappings
// are committed as they happen, and the pipeline cache is a crash-safe SQLite database.

#include "restart.hpp"
#include "live_apply.hpp"

#include "disc.hpp"
#include "mppm/launcher.hpp"
#include "mods_window.hpp"
#include "saves_window.hpp"
#include "settings.hpp"

#include <aurora/dvd.h>
#include <borealis/config.hpp>
#include <p2_net.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#else
#include <fcntl.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <fstream>
#include <iterator>
#endif
#endif

// The AURORA_EXIT teardown (port/main.cpp): park the other game threads and the scheduler's timer,
// close the audio device. Both are safe before the game has started.
extern "C" void p2_os_shutdown();
extern "C" void p2_audio_shutdown();

namespace mppm::launcher {
namespace {

constexpr std::string_view kPlayFlag = "--play";
constexpr std::string_view kLauncherFlag = "--launcher";

// The arguments after the program name, as the process was started.
std::vector<std::string> original_arguments() {
    std::vector<std::string> args;
#if defined(_WIN32)
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; i++) {
            const int len = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
            std::string arg(len > 0 ? len - 1 : 0, '\0');
            if (len > 1) {
                WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, arg.data(), len, nullptr, nullptr);
            }
            args.push_back(std::move(arg));
        }
        LocalFree(argv);
    }
#elif defined(__APPLE__)
    const int argc = *_NSGetArgc();
    char** argv = *_NSGetArgv();
    for (int i = 1; i < argc; i++) {
        args.emplace_back(argv[i] != nullptr ? argv[i] : "");
    }
#elif defined(__linux__)
    // NUL-separated, with a trailing NUL after the last argument.
    std::ifstream file("/proc/self/cmdline", std::ios::binary);
    const std::string all{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    std::size_t start = 0;
    bool first = true;
    while (start < all.size()) {
        std::size_t end = all.find('\0', start);
        if (end == std::string::npos) {
            end = all.size();
        }
        if (!first) {
            args.push_back(all.substr(start, end - start));
        }
        first = false;
        start = end + 1;
    }
#endif
    return args;
}

// Whether a --cvar assignment ("key=value") still holds its var: a choice made since in Settings
// released it from the command-line layer, and must not be reverted by the next process.
bool cvar_still_held(std::string_view assignment) {
    const auto eq = assignment.find('=');
    const Meta* m = eq != std::string_view::npos ? find_meta(assignment.substr(0, eq)) : nullptr;
    return m == nullptr || cvar_overlay().contains(*m->var); // unknown keys stay as they were
}

// The original arguments with the restart target as the only --play / --launcher.
std::vector<std::string> restart_arguments(RestartTarget target) {
    std::vector<std::string> args = original_arguments();
    std::erase_if(args, [](const std::string& arg) { return arg == kPlayFlag || arg == kLauncherFlag; });
    // A disc chosen since start (and usable) replaces the one on the command line or in MPPM_DISC
    // (live_options_design 3.8): the positional disc is only ever the first argument.
    const auto pending = settings_restart_pending(true);
    const std::string& chosen = settings().backend.discPath.get();
    if (std::ranges::find(pending, &settings().backend.discPath) != pending.end() && !chosen.empty() &&
        disc::inspect(chosen).state == disc::State::Ready)
    {
        if (!args.empty() && !args.front().empty() && args.front().front() != '-') {
            args.erase(args.begin());
        }
#if defined(_WIN32)
        _putenv_s("MPPM_DISC", "");
#else
        unsetenv("MPPM_DISC");
#endif
    }
    // --cvar key=value / --cvar=key=value whose var a choice in Settings has since released.
    for (std::size_t i = 0; i < args.size();) {
        if (args[i] == "--cvar" && i + 1 < args.size()) {
            if (!cvar_still_held(args[i + 1])) {
                args.erase(args.begin() + static_cast<std::ptrdiff_t>(i), args.begin() + static_cast<std::ptrdiff_t>(i) + 2);
                continue;
            }
            i += 2;
            continue;
        }
        if (args[i].starts_with("--cvar=") && !cvar_still_held(std::string_view(args[i]).substr(7))) {
            args.erase(args.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        i++;
    }
    if (target == RestartTarget::SameAsNow) {
        target = prelaunch_open() ? RestartTarget::StartMenu : RestartTarget::Play;
    }
    args.emplace_back(target == RestartTarget::Play ? kPlayFlag : kLauncherFlag);
    return args;
}

#if defined(_WIN32)

// Quotes one argument the way CommandLineToArgvW reads it back.
std::wstring quote_argument(const std::string& utf8) {
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring arg(len > 0 ? len - 1 : 0, L'\0');
    if (len > 1) {
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, arg.data(), len);
    }
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring out = L"\"";
    for (std::size_t i = 0;; i++) {
        std::size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            i++;
            backslashes++;
        }
        if (i == arg.size()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
        } else {
            out.append(backslashes, L'\\');
        }
        out.push_back(arg[i]);
    }
    out.push_back(L'"');
    return out;
}

void relaunch(const std::vector<std::string>& args) {
    wchar_t exe[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    if (n == 0 || n >= std::size(exe)) {
        std::fprintf(stderr, "mppm: restart failed: cannot find the executable\n");
        return;
    }
    std::wstring commandLine = L"\"" + std::wstring(exe, n) + L"\"";
    for (const std::string& arg : args) {
        commandLine += L" " + quote_argument(arg);
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe, commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
            &startup, &process))
    {
        std::fprintf(stderr, "mppm: restart failed: CreateProcessW error %lu\n", GetLastError());
        return;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    std::fflush(nullptr);
    std::_Exit(0);
}

#else

// Path of the running binary. On Linux a binary replaced by a rebuild reads as "<path> (deleted)":
// prefer the new file at that path, which is what a developer who just rebuilt expects.
std::string executable_path() {
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    if (size > 0 && _NSGetExecutablePath(path.data(), &size) == 0) {
        path.resize(std::strlen(path.c_str()));
        return path;
    }
    return {};
#elif defined(__linux__)
    char buf[4096];
    const ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) {
        return {};
    }
    std::string path(buf, static_cast<std::size_t>(len));
    constexpr std::string_view kDeleted = " (deleted)";
    if (path.ends_with(kDeleted)) {
        path.resize(path.size() - kDeleted.size());
    }
    return path;
#else
    return {};
#endif
}

// Nothing open in this process leaks into the next one: the window system connection, the GPU and
// audio devices and every file are closed by exec.
void close_descriptors_on_exec() {
#if defined(__linux__) && defined(CLOSE_RANGE_CLOEXEC)
    if (close_range(3, ~0U, CLOSE_RANGE_CLOEXEC) == 0) {
        return;
    }
#endif
    long maxFd = sysconf(_SC_OPEN_MAX);
    if (maxFd < 0 || maxFd > 65536) {
        maxFd = 65536;
    }
    for (int fd = 3; fd < maxFd; fd++) {
        const int flags = fcntl(fd, F_GETFD);
        if (flags >= 0) {
            fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
        }
    }
}

void relaunch(const std::vector<std::string>& args) {
    const std::string exe = executable_path();
    std::vector<std::string> argvStorage;
    argvStorage.reserve(args.size() + 1);
    argvStorage.push_back(exe.empty() ? std::string{"mppm"} : exe);
    argvStorage.insert(argvStorage.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& arg : argvStorage) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    close_descriptors_on_exec();
    std::fflush(nullptr);
    if (!exe.empty()) {
        execv(exe.c_str(), argv.data());
    }
#if defined(__linux__)
    // The file at that path is gone or not executable: run the image this process came from.
    execv("/proc/self/exe", argv.data());
#endif
    std::fprintf(stderr, "mppm: restart failed: %s\n", std::strerror(errno));
}

#endif

} // namespace

void request_restart(RestartTarget target) {
    const std::vector<std::string> args = restart_arguments(target);
    // Settings first: the next process must start with the change that asked for the restart.
    if (const auto status = borealis::config::Registry::global().flush(); !status) {
        std::fprintf(stderr, "mppm: settings: %s\n", status.message.c_str());
    }
    if (p2_net_active()) {
        // Harness run (M5.1a): the new process would get the same --record / --checksum-log and
        // truncate this run's logs. End the session (E line, tick count) and quit instead.
        std::fprintf(stderr, "mppm: net: restart requested in a harness run: ending the run and quitting instead\n");
        p2_net_shutdown();
        const int code = p2_net_exit_code();
        p2_os_shutdown();
        p2_audio_shutdown();
        aurora_dvd_close();
        std::fflush(nullptr);
        std::_Exit(code);
    }
    p2_os_shutdown();
    p2_audio_shutdown();
    aurora_dvd_close();
    relaunch(args);
    // Only reached when the new process could not be started. The game threads are parked and the
    // audio device is closed, so carrying on would leave a frozen, silent game: leave instead, as
    // the window's close button would.
    std::fflush(nullptr);
    std::_Exit(1);
}

namespace {
bool sGameBooted = false; // note_game_booted ran: the game boots behind the start menu
} // namespace

bool game_booted() {
    // In game, the start menu is closed. With the start menu open the game has booted behind it
    // only once the boot flow said so (live menu, README 3); the standalone menu boots on Play.
    return sGameBooted || !prelaunch_open();
}

void note_game_booted() {
    if (!sGameBooted) {
        sGameBooted = true;
        live::on_game_booted();
    }
}

bool restart_pending() {
    // The Save Manager changing the card of a game that already read it counts too (SETTINGS 10), and
    // so does a mod list that differs from the one the boot mounted (SETTINGS 17, the Mods window).
    if (prelaunch_open()) {
        // On the start menu every other change is applied as it is made (live_apply): only a
        // relaunch still waiting for its moment counts, not a boot-time key or an unusable disc.
        return live::pending_relaunch() || ui::saves_restart_pending() || ui::mods_restart_pending();
    }
    return !settings_restart_pending(game_booted()).empty() || ui::saves_restart_pending() ||
           ui::mods_restart_pending();
}

} // namespace mppm::launcher
