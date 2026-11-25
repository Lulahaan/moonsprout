#include "saves_window.hpp"
#include "p2_data_dir.hpp"

#include "branding.hpp"
#include "disc.hpp"
#include "settings.hpp"

#include "ui/button.hpp"
#include "ui/format.hpp"
#include "ui/icon_button.hpp"
#include "ui/modal.hpp"
#include "ui/pane.hpp"
#include "ui/ui.hpp"

#include <aurora/card.h>
#include <aurora/lib/window.hpp>
#include <borealis/file_select.hpp>
#include <borealis/io.hpp>
#include <fmt/format.h>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_misc.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace mppm::ui {
namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------------------------
// Card files (model). Everything here runs on the UI thread; card files are small (Pikmin 2
// keeps its three story slots in one 27-block file), so nothing needs a worker.
// ---------------------------------------------------------------------------------------------

constexpr const char* kGame  = "GPVE"; // game code of the North American release
constexpr const char* kMaker = "01";   // Nintendo
constexpr size_t kGciHeaderSize = 64;  // the directory entry in front of a .gci
constexpr size_t kBlockSize     = 8192;
constexpr size_t kMaxImportSize = 32u << 20; // a full 2043-block card is 16 MiB
constexpr size_t kAutoBackupsKept = 10;
// GameCube time counts seconds from 2000-01-01, in local time.
constexpr std::chrono::sys_days kGameCubeEpoch = std::chrono::year{2000} / 1 / 1;

struct Status {
    bool ok = true;
    std::string message;
    explicit operator bool() const { return ok; }
};

Status fail(std::string message) {
    return {false, std::move(message)};
}

// Where saves live: the format the game mounted, else the configured one (before the boot).
struct Storage {
    AuroraCardType type = AURORA_CARD_GCI_DIRECTORY;
    fs::path path;        // GCI folder, or the .raw file
    bool mounted = false; // the game has called CARDInit: changes need a remount and a restart
};

Storage active_storage() {
    Storage storage;
    const AuroraCardType mounted = aurora_card_get_type(0);
    storage.mounted = mounted != AURORA_CARD_UNAVAILABLE;
    if (storage.mounted) {
        storage.type = mounted;
    } else {
        storage.type = settings().backend.cardFileType.get() == CardFileType::CardImage ? AURORA_CARD_RAW_IMAGE
                                                                                       : AURORA_CARD_GCI_DIRECTORY;
    }
    std::string buffer(aurora_card_get_path(kGame, storage.type, 0, nullptr, 0), '\0');
    if (!buffer.empty() && aurora_card_get_path(kGame, storage.type, 0, buffer.data(), buffer.size()) != 0) {
        buffer.resize(std::strlen(buffer.c_str()));
        storage.path = borealis::io::fs_path_from_utf8(buffer);
    }
    return storage;
}

const char* storage_label(const Storage& storage) {
    return storage.type == AURORA_CARD_RAW_IMAGE ? "Memory card image" : "GCI folder";
}

fs::path data_dir() {
    const std::string& dir = mppm::data_dir_utf8();
    return dir.empty() ? fs::path(".") : borealis::io::fs_path_from_utf8(dir);
}

fs::path backups_dir() {
    return data_dir() / "backups";
}

using Bytes = std::vector<uint8_t>;

uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
uint16_t be16(const uint8_t* p) {
    return uint16_t((p[0] << 8) | p[1]);
}

// A fixed-size field up to its first NUL, with anything unprintable dropped (US saves are ASCII).
std::string c_field(const uint8_t* p, size_t size) {
    std::string out;
    for (size_t i = 0; i < size && p[i] != 0; ++i) {
        if (p[i] >= 0x20 && p[i] < 0x7F) {
            out.push_back(static_cast<char>(p[i]));
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

// One card file, as a .gci (directory entry + blocks).
struct SaveFile {
    std::string game;     // "GPVE"
    std::string maker;    // "01"
    std::string fileName; // name inside the card, e.g. "Pikmin2_SaveData"
    std::string title;    // the card comment, e.g. "PIKMIN 2"
    std::string detail;   // second comment line; Pikmin 2 writes the save time there
    uint32_t modTime = 0; // seconds since 2000-01-01, local time
    uint16_t blocks  = 0;
    uint64_t size    = 0;
    fs::path path;        // GCI folder: the file on disk; card image: empty

    std::string gci_name() const {
        std::array<char, 64> buffer{};
        if (aurora_card_gci_filename(game.c_str(), maker.c_str(), fileName.c_str(), buffer.data(), buffer.size()) == 0) {
            return fileName + ".gci";
        }
        return buffer.data();
    }
    bool ours() const { return game == kGame && maker == kMaker; }
};

// Reads the directory entry of a .gci. Rejects anything whose size doesn't match its block count.
std::optional<SaveFile> parse_gci(std::span<const uint8_t> bytes) {
    if (bytes.size() < kGciHeaderSize + kBlockSize) {
        return std::nullopt;
    }
    const uint8_t* h = bytes.data();
    SaveFile file;
    file.game     = std::string(reinterpret_cast<const char*>(h), 4);
    file.maker    = std::string(reinterpret_cast<const char*>(h + 4), 2);
    file.fileName = c_field(h + 0x08, 32);
    file.modTime  = be32(h + 0x28);
    file.blocks   = be16(h + 0x38);
    file.size     = bytes.size();
    const auto printable = [](const std::string& s) {
        return std::ranges::all_of(s, [](char c) { return c > 0x20 && c < 0x7F; });
    };
    if (!printable(file.game) || !printable(file.maker) || file.fileName.empty() || file.blocks == 0 ||
        bytes.size() != kGciHeaderSize + size_t(file.blocks) * kBlockSize)
    {
        return std::nullopt;
    }
    const uint32_t comment = be32(h + 0x3C);
    if (comment != 0xFFFFFFFFu && kGciHeaderSize + size_t(comment) + 64 <= bytes.size()) {
        file.title  = c_field(h + kGciHeaderSize + comment, 32);
        file.detail = c_field(h + kGciHeaderSize + comment + 32, 32);
    }
    return file;
}

std::optional<Bytes> read_file(const fs::path& path, size_t limit = kMaxImportSize) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || size > limit) {
        return std::nullopt;
    }
    std::ifstream in(path, std::ios::binary);
    Bytes bytes(size);
    if (!in || !in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size))) {
        return std::nullopt;
    }
    return bytes;
}

// Writes next to the target and renames over it, so a failed write never leaves half a save.
Status write_file(const fs::path& path, std::span<const uint8_t> bytes) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out || !out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size())) ||
            !out.flush())
        {
            fs::remove(temp, ec);
            return fail(fmt::format("Couldn't write {}.", borealis::io::fs_path_to_string(path.filename())));
        }
    }
    fs::rename(temp, path, ec);
    if (ec) {
        fs::remove(temp, ec);
        return fail(fmt::format("Couldn't write {}.", borealis::io::fs_path_to_string(path.filename())));
    }
    return {};
}

// The .gci bytes of a file in a card image.
std::optional<Bytes> extract_raw(const fs::path& image, const std::string& fileName) {
    const std::string imagePath = borealis::io::fs_path_to_string(image);
    const size_t size = aurora_card_raw_extract(imagePath.c_str(), kGame, kMaker, fileName.c_str(), nullptr, 0);
    if (size == 0) {
        return std::nullopt;
    }
    Bytes bytes(size);
    if (aurora_card_raw_extract(imagePath.c_str(), kGame, kMaker, fileName.c_str(), bytes.data(), bytes.size()) != size) {
        return std::nullopt;
    }
    return bytes;
}

// The Pikmin 2 file names in a card image; nullopt when the image can't be read.
std::optional<std::vector<std::string>> list_raw(const fs::path& image) {
    std::vector<std::string> names;
    const std::string imagePath = borealis::io::fs_path_to_string(image);
    const bool ok = aurora_card_raw_list(imagePath.c_str(), kGame, kMaker,
                                         [](const char* name, void* user) {
                                             static_cast<std::vector<std::string>*>(user)->emplace_back(name);
                                         },
                                         &names);
    if (!ok) {
        return std::nullopt;
    }
    return names;
}

struct SaveList {
    Status status;
    std::vector<SaveFile> files;
};

SaveList list_saves(const Storage& storage) {
    SaveList list;
    std::error_code ec;
    if (storage.path.empty()) {
        list.status = fail("The save location is unknown.");
        return list;
    }
    if (storage.type == AURORA_CARD_GCI_DIRECTORY) {
        if (!fs::is_directory(storage.path, ec)) {
            return list; // no save yet
        }
        for (const auto& entry : fs::directory_iterator(storage.path, ec)) {
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".gci") {
                continue;
            }
            const auto bytes = read_file(entry.path());
            auto file        = bytes ? parse_gci(*bytes) : std::nullopt;
            if (file && file->ours()) {
                file->path = entry.path();
                list.files.push_back(std::move(*file));
            }
        }
    } else {
        if (!fs::is_regular_file(storage.path, ec)) {
            return list; // no card image yet
        }
        const auto names = list_raw(storage.path);
        if (!names) {
            list.status = fail("The memory card image can't be read. It may be damaged.");
            return list;
        }
        for (const auto& name : *names) {
            const auto bytes = extract_raw(storage.path, name);
            if (auto file = bytes ? parse_gci(*bytes) : std::nullopt) {
                list.files.push_back(std::move(*file));
            }
        }
    }
    std::ranges::sort(list.files, {}, &SaveFile::fileName);
    return list;
}

std::optional<Bytes> read_save(const Storage& storage, const SaveFile& file) {
    if (storage.type == AURORA_CARD_GCI_DIRECTORY) {
        return read_file(file.path);
    }
    return extract_raw(storage.path, file.fileName);
}

// Adds or replaces one card file.
Status install_gci(const Storage& storage, std::span<const uint8_t> gci) {
    const auto file = parse_gci(gci);
    if (!file) {
        return fail("The save file is damaged.");
    }
    if (storage.type == AURORA_CARD_GCI_DIRECTORY) {
        return write_file(storage.path / file->gci_name(), gci);
    }
    std::error_code ec;
    fs::create_directories(storage.path.parent_path(), ec);
    const std::string imagePath = borealis::io::fs_path_to_string(storage.path);
    if (!aurora_card_raw_insert(imagePath.c_str(), gci.data(), gci.size(), true)) {
        return fail("The save couldn't be written to the memory card image. The card may be full or damaged.");
    }
    return {};
}

Status remove_save(const Storage& storage, const SaveFile& file) {
    if (storage.type == AURORA_CARD_GCI_DIRECTORY) {
        std::error_code ec;
        if (!fs::remove(file.path, ec) || ec) {
            return fail("The save file couldn't be deleted.");
        }
        return {};
    }
    const std::string imagePath = borealis::io::fs_path_to_string(storage.path);
    if (!aurora_card_raw_delete(imagePath.c_str(), kGame, kMaker, file.fileName.c_str())) {
        return fail("The save couldn't be removed from the memory card image.");
    }
    return {};
}

// ---- Backups ----

struct Backup {
    std::string name; // folder name: "2026-11-21_035255" (a "-2" suffix for a second one that second)
    fs::path path;
    bool automatic = false;
    std::string reason;
    std::vector<SaveFile> files; // path set to the .gci inside the backup
};

std::string local_stamp() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    std::array<char, 32> buffer{};
    std::strftime(buffer.data(), buffer.size(), "%Y-%m-%d_%H%M%S", &tm);
    return buffer.data();
}

constexpr std::array kMonths{"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

std::string format_civil(int year, unsigned month, unsigned day, int hour, int minute) {
    if (month < 1 || month > 12) {
        return {};
    }
    return fmt::format("{} {}, {} \xC2\xB7 {:02}:{:02}", kMonths[month - 1], day, year, hour, minute);
}

// "Nov 21, 2026 · 03:52" from a backup folder name; the name itself when it isn't a stamp.
std::string backup_label(const std::string& name) {
    int year = 0;
    unsigned month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (std::sscanf(name.c_str(), "%4d-%2u-%2u_%2u%2u%2u", &year, &month, &day, &hour, &minute, &second) == 6) {
        if (auto label = format_civil(year, month, day, int(hour), int(minute)); !label.empty()) {
            return label;
        }
    }
    return name;
}

std::string format_gc_time(uint32_t seconds) {
    using namespace std::chrono;
    const sys_seconds time = kGameCubeEpoch + std::chrono::seconds{seconds};
    const auto days = floor<std::chrono::days>(time);
    const year_month_day date{days};
    const hh_mm_ss clock{time - days};
    return format_civil(int(date.year()), unsigned(date.month()), unsigned(date.day()), int(clock.hours().count()),
                        int(clock.minutes().count()));
}

std::vector<Backup> list_backups() {
    std::vector<Backup> backups;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(backups_dir(), ec)) {
        if (!entry.is_directory(ec)) {
            continue;
        }
        Backup backup;
        backup.path = entry.path();
        backup.name = borealis::io::fs_path_to_string(entry.path().filename());
        if (backup.name.starts_with('.')) {
            continue; // a backup still being written
        }
        if (std::ifstream manifest(entry.path() / "backup.txt"); manifest) {
            for (std::string line; std::getline(manifest, line);) {
                if (line == "kind=auto") {
                    backup.automatic = true;
                } else if (line.starts_with("reason=")) {
                    backup.reason = line.substr(7);
                }
            }
        }
        for (const auto& file : fs::directory_iterator(entry.path(), ec)) {
            if (file.path().extension() != ".gci") {
                continue;
            }
            const auto bytes = read_file(file.path());
            if (auto save = bytes ? parse_gci(*bytes) : std::nullopt; save && save->ours()) {
                save->path = file.path();
                backup.files.push_back(std::move(*save));
            }
        }
        if (!backup.files.empty()) {
            backups.push_back(std::move(backup));
        }
    }
    // Stamps sort by time; newest first.
    std::ranges::sort(backups, std::greater{}, &Backup::name);
    return backups;
}

void prune_auto_backups() {
    size_t kept = 0;
    for (const auto& backup : list_backups()) {
        if (backup.automatic && ++kept > kAutoBackupsKept) {
            std::error_code ec;
            fs::remove_all(backup.path, ec);
        }
    }
}

// Copies every Pikmin 2 save of the card into a new backup folder. With no saves there is
// nothing to keep: success, and no folder.
Status create_backup(const Storage& storage, bool automatic, const std::string& reason) {
    const auto saves = list_saves(storage);
    if (!saves.status) {
        return saves.status;
    }
    if (saves.files.empty()) {
        return automatic ? Status{} : fail("There is no save to back up.");
    }
    std::error_code ec;
    const fs::path root = backups_dir();
    const std::string stamp = local_stamp();
    std::string name = stamp;
    for (int n = 2; fs::exists(root / name, ec); ++n) {
        name = fmt::format("{}-{}", stamp, n);
    }
    // Written under a dot name, then renamed: a half-written backup never shows in the list.
    const fs::path staging = root / ("." + name);
    fs::remove_all(staging, ec);
    if (!fs::create_directories(staging, ec)) {
        return fail("The backup folder couldn't be created.");
    }
    const auto abort = [&](std::string message) {
        fs::remove_all(staging, ec);
        return fail(std::move(message));
    };
    for (const auto& file : saves.files) {
        const auto bytes = read_save(storage, file);
        if (!bytes) {
            return abort(fmt::format("{} couldn't be read.", file.fileName));
        }
        if (auto status = write_file(staging / file.gci_name(), *bytes); !status) {
            return abort(status.message);
        }
    }
    {
        std::ofstream manifest(staging / "backup.txt", std::ios::trunc);
        manifest << "kind=" << (automatic ? "auto" : "manual") << '\n';
        if (!reason.empty()) {
            manifest << "reason=" << reason << '\n';
        }
        manifest << "format=" << (storage.type == AURORA_CARD_RAW_IMAGE ? "card-image" : "gci-folder") << '\n';
    }
    fs::rename(staging, root / name, ec);
    if (ec) {
        return abort("The backup couldn't be saved.");
    }
    if (automatic) {
        prune_auto_backups();
    }
    return {};
}

// ---- Change bookkeeping ----

std::atomic_uint64_t sGeneration = 1; // bumped after every change; open windows rebuild
bool sRestartPending = false;

// After the card changed: tell a booted game (remount + restart pending) and refresh the window.
void card_changed(const Storage& storage) {
    if (storage.mounted) {
        aurora_card_remount(0);
        sRestartPending = true;
    }
    ++sGeneration;
}

// Runs `action` after an automatic backup. A failed backup stops before anything is changed.
Status with_backup(const Storage& storage, const std::string& reason, const std::function<Status()>& action) {
    if (auto status = create_backup(storage, true, reason); !status) {
        return fail(fmt::format("Your current save couldn't be backed up, so nothing was changed. {}", status.message));
    }
    ++sGeneration; // the new backup shows even if the action fails
    return action();
}

// ---------------------------------------------------------------------------------------------
// Dialog helpers
// ---------------------------------------------------------------------------------------------

void dismiss(Modal& modal) {
    play_sound(kSoundWindowClose);
    modal.pop();
}

void push_modal(std::unique_ptr<Modal> modal) {
    if (auto* host = top_document()) {
        host->push(std::move(modal));
    } else {
        push_document(std::move(modal));
    }
}

void show_message(Rml::String title, Rml::String body, bool error = false) {
    push_modal(std::make_unique<Modal>(Modal::Props{
        .title     = std::move(title),
        .bodyText  = std::move(body),
        .actions   = {{.label = "OK", .onPressed = &dismiss}},
        .onDismiss = &dismiss,
        .icon      = error ? "warning" : "",
    }));
}

void show_result(const Rml::String& title, const Rml::String& success, const Status& status) {
    if (status) {
        show_message(title, success);
    } else {
        show_message(title, status.message, true);
    }
}

// Asks before a destructive action. `run` happens after the dialog is gone.
void confirm(Rml::String title, Rml::String body, Rml::String verb, std::function<void()> run) {
    push_modal(std::make_unique<Modal>(Modal::Props{
        .title    = std::move(title),
        .bodyText = std::move(body),
        .actions =
            {
                {.label = "Cancel", .onPressed = &dismiss},
                {.label     = std::move(verb),
                 .onPressed = [run = std::move(run)](Modal& modal) {
                     modal.pop();
                     run();
                 }},
            },
        .onDismiss = &dismiss,
        .icon      = "warning",
    }));
}

// A friendly name for a card file. Pikmin 2 has one, holding all three story slots.
std::string save_label(const SaveFile& file) {
    if (file.fileName == "Pikmin2_SaveData") {
        return "Pikmin 2 save";
    }
    return file.fileName;
}

// When the save was written. Pikmin 2 puts "MM/DD/YYYY HH:MM:SS" (the console clock) in the second
// comment line; that is preferred over the directory time, which the port's clock may skew.
std::string save_time(const SaveFile& file) {
    int year = 0;
    unsigned month = 0, day = 0, hour = 0, minute = 0;
    if (std::sscanf(file.detail.c_str(), "%2u/%2u/%4d %2u:%2u", &month, &day, &year, &hour, &minute) == 5) {
        if (auto label = format_civil(year, month, day, int(hour), int(minute)); !label.empty()) {
            return label;
        }
    }
    return file.modTime != 0 ? format_gc_time(file.modTime) : std::string{};
}

// ---- Import ----

// Installs the imported files after an automatic backup.
void perform_import(std::vector<Bytes> files) {
    const Storage storage = active_storage();
    size_t done = 0;
    const Status status = with_backup(storage, "Before importing a save", [&]() -> Status {
        for (const auto& gci : files) {
            if (auto result = install_gci(storage, gci); !result) {
                return result;
            }
            ++done;
        }
        return {};
    });
    if (done != 0) {
        card_changed(storage);
    }
    show_result("Import save", done > 1 ? fmt::format("{} saves were imported.", done) : "The save was imported.", status);
}

// Checks the imported files against the card and asks before replacing anything.
void confirm_import(std::vector<Bytes> files) {
    const Storage storage = active_storage();
    const auto current = list_saves(storage);
    if (!current.status) {
        show_message("Import save", current.status.message, true);
        return;
    }
    bool replacing = false;
    for (const auto& gci : files) {
        const auto file = parse_gci(gci);
        replacing |= file && std::ranges::any_of(current.files, [&](const SaveFile& f) { return f.fileName == file->fileName; });
    }
    if (!replacing) {
        perform_import(std::move(files));
        return;
    }
    confirm("Replace save",
            "A Pikmin 2 save already exists. Replace it with the imported one? The current save is backed up first.",
            "Replace", [files = std::move(files)]() mutable { perform_import(std::move(files)); });
}

// The Pikmin 2 files inside a Dolphin card image, as .gci bytes.
std::optional<std::vector<Bytes>> saves_from_image(std::span<const uint8_t> image, std::string& error) {
    // Aurora reads card images from a path: stage the bytes (the picked location may not be one).
    const fs::path staging = data_dir() / ".import.raw";
    if (auto status = write_file(staging, image); !status) {
        error = status.message;
        return std::nullopt;
    }
    std::vector<Bytes> files;
    const auto names = list_raw(staging);
    if (names) {
        for (const auto& name : *names) {
            if (auto bytes = extract_raw(staging, name)) {
                files.push_back(std::move(*bytes));
            }
        }
    }
    std::error_code ec;
    fs::remove(staging, ec);
    if (!names) {
        error = "This file isn't a GameCube memory card image, or it is damaged.";
        return std::nullopt;
    }
    if (files.empty()) {
        error = "This memory card image has no Pikmin 2 (USA) save on it.";
        return std::nullopt;
    }
    return files;
}

void import_location(const std::string& location) {
    auto opened = borealis::io::open(location);
    if (opened.status != borealis::io::Status::Ok) {
        show_message("Import save", "The file couldn't be opened.", true);
        return;
    }
    const uint64_t size = opened.file.size();
    if (size == 0 || size > kMaxImportSize) {
        show_message("Import save", "This file isn't a GameCube save.", true);
        return;
    }
    Bytes bytes(size);
    if (opened.file.read(bytes.data(), size) != size) {
        show_message("Import save", "The file couldn't be read.", true);
        return;
    }

    std::vector<Bytes> files;
    if (const auto gci = parse_gci(bytes)) {
        if (!gci->ours()) {
            const std::string id = gci->game + gci->maker;
            show_message("Import save",
                         gci->game.starts_with("GPV")
                             ? fmt::format("This save is from another Pikmin 2 release ({}). {} plays the North "
                                           "American disc, whose saves are GPVE01.",
                                           id, branding::kProductName)
                             : fmt::format("This is a save for another game ({}), not Pikmin 2.", id),
                         true);
            return;
        }
        files.push_back(std::move(bytes));
    } else if (size % kBlockSize == 0) {
        std::string error;
        auto extracted = saves_from_image(bytes, error);
        if (!extracted) {
            show_message("Import save", error, true);
            return;
        }
        files = std::move(*extracted);
    } else {
        show_message("Import save", "This file isn't a GameCube save. Pick a .gci file or a Dolphin .raw memory card.", true);
        return;
    }
    confirm_import(std::move(files));
}

void begin_import() {
    if (borealis::file_select::busy()) {
        return;
    }
    borealis::file_select::FileOptions options;
    options.parentWindow = aurora::window::get_sdl_window();
    options.filters      = {{"GameCube saves", "gci;raw"}, {"All files", "*"}};
    borealis::file_select::open_file(std::move(options), [](borealis::file_select::Result result) {
        if (result.status == borealis::file_select::Status::Selected && !result.locations.empty()) {
            import_location(result.locations.front());
        } else if (result.status == borealis::file_select::Status::Failed) {
            show_message("Import save", "The file picker couldn't be opened.", true);
        }
    });
}

// ---- Export ----

void begin_export(const SaveFile& file) {
    if (borealis::file_select::busy()) {
        return;
    }
    const Storage storage = active_storage();
    fs::path source = file.path;
    bool temporary  = false;
    if (storage.type == AURORA_CARD_RAW_IMAGE) {
        // Card image: extract the file first; the copy is removed once the dialog closes.
        const auto bytes = read_save(storage, file);
        source = data_dir() / ".export.gci";
        if (!bytes || !write_file(source, *bytes)) {
            show_message("Export save", "The save couldn't be read from the memory card image.", true);
            return;
        }
        temporary = true;
    }
    borealis::file_select::ExportOptions options;
    options.parentWindow   = aurora::window::get_sdl_window();
    options.sourceLocation = borealis::io::fs_path_to_string(source);
    options.suggestedName  = file.gci_name();
    options.filters        = {{"GameCube save", "gci"}};
    borealis::file_select::export_file(std::move(options), [source, temporary](borealis::file_select::Result result) {
        if (temporary) {
            std::error_code ec;
            fs::remove(source, ec);
        }
        if (result.status == borealis::file_select::Status::Selected) {
            show_message("Export save", "The save was exported.");
        } else if (result.status == borealis::file_select::Status::Failed) {
            show_message("Export save", "The save couldn't be exported.", true);
        }
    });
}

// ---- Delete, back up, restore ----

void begin_delete(const SaveFile& file) {
    confirm("Delete save", "Delete this save? A backup is kept in the Backups list.", "Delete", [file] {
        const Storage storage = active_storage();
        const Status status = with_backup(storage, "Before deleting a save", [&] { return remove_save(storage, file); });
        if (status) {
            card_changed(storage);
        } else {
            show_message("Delete save", status.message, true);
        }
    });
}

void back_up_now() {
    const Status status = create_backup(active_storage(), false, {});
    if (status) {
        ++sGeneration;
    }
    show_result("Back up", "A backup of your save was made. It is never removed automatically.", status);
}

void begin_restore(const Backup& backup) {
    const std::string when = backup_label(backup.name);
    confirm("Restore backup",
            fmt::format("Replace your current saves with the backup from {}? Your current saves are backed up first.", when),
            "Restore", [backup, when] {
                const Storage storage = active_storage();
                bool changed = false;
                const Status status = with_backup(storage, fmt::format("Before restoring the backup from {}", when), [&]() -> Status {
                    const auto current = list_saves(storage);
                    if (!current.status) {
                        return current.status;
                    }
                    for (const auto& file : current.files) {
                        if (auto result = remove_save(storage, file); !result) {
                            return result;
                        }
                        changed = true;
                    }
                    for (const auto& file : backup.files) {
                        const auto bytes = read_file(file.path);
                        if (!bytes) {
                            return fail(fmt::format("{} couldn't be read from the backup.", file.fileName));
                        }
                        if (auto result = install_gci(storage, *bytes); !result) {
                            return result;
                        }
                    }
                    return {};
                });
                if (changed || status) {
                    card_changed(storage);
                }
                show_result("Restore backup", "The backup was restored.",
                            status ? status
                                   : fail(status.message + " Your previous saves are in the newest automatic backup."));
            });
}

void begin_delete_backup(const Backup& backup) {
    confirm("Delete backup", fmt::format("Delete the backup from {}? This can't be undone.", backup_label(backup.name)),
            "Delete", [path = backup.path] {
                std::error_code ec;
                fs::remove_all(path, ec);
                ++sGeneration;
                if (ec) {
                    show_message("Delete backup", "The backup couldn't be deleted.", true);
                }
            });
}

void open_folder(const fs::path& folder) {
    std::error_code ec;
    fs::create_directories(folder, ec);
    const std::string url = "file://" + borealis::io::fs_path_to_generic_string(folder);
    if (!SDL_OpenURL(url.c_str())) {
        show_message("Open folder", "The folder couldn't be opened in the file manager.", true);
    }
}

fs::path save_folder(const Storage& storage) {
    return storage.type == AURORA_CARD_GCI_DIRECTORY ? storage.path : storage.path.parent_path();
}

// ---------------------------------------------------------------------------------------------
// Window parts
// ---------------------------------------------------------------------------------------------

const char* kBackupHelp =
    "An automatic backup is made before every import, restore and delete. The newest 10 are kept; "
    "backups you make yourself are never removed.";

void append_header(Rml::Element* parent, const Rml::String& title, const Rml::String& subtitle) {
    auto* header = append(parent, "save-header");
    append_text_element(header, "heading", title);
    append_text_element(header, "small", subtitle);
}

void build_save_detail(Pane& pane, const Storage& storage, const SaveFile& file) {
    append_header(pane.root(), save_label(file), fmt::format("{} \xC2\xB7 Card A", storage_label(storage)));

    auto* overview = append(pane.root(), "save-overview");
    overview->SetClass("present", true);
    const std::string when = save_time(file);
    append_text_element(overview, "heading", when.empty() ? "Save present" : "Saved " + when);
    std::string facts = fmt::format("{} \xC2\xB7 {} blocks", format_bytes(file.size), file.blocks);
    if (file.fileName == "Pikmin2_SaveData") {
        facts += " \xC2\xB7 holds all three story files";
    }
    append_text_element(overview, "small", facts);

    pane.add_section("Transfer");
    append_text_element(pane.root(), "small", "Export a .gci file for Dolphin, a GameCube, or another copy.");
    pane.add_button(ControlledButton::Props{
                        .text       = "Export Save...",
                        .isDisabled = [] { return borealis::file_select::busy(); },
                    })
        .on_pressed([file] { begin_export(file); });

    pane.add_section("Backups");
    append_text_element(pane.root(), "small", kBackupHelp);
    pane.add_button("Back Up Now").on_pressed(&back_up_now);
    pane.add_button("Open Save Folder").on_pressed([folder = save_folder(storage)] { open_folder(folder); });
    append_text_element(pane.root(), "file-path",
                        borealis::io::fs_path_to_string(file.path.empty() ? storage.path : file.path));

    pane.add_section("Danger Zone");
    auto& remove = pane.add_button("Delete Save...");
    remove.root()->SetClass("danger", true);
    remove.on_pressed([file] { begin_delete(file); });
}

void build_backup_detail(Pane& pane, const Backup& backup) {
    append_header(pane.root(), backup_label(backup.name),
                  backup.automatic ? (backup.reason.empty() ? std::string("Automatic backup")
                                                            : fmt::format("Automatic backup \xC2\xB7 {}", backup.reason))
                                   : std::string("Manual backup \xC2\xB7 never removed automatically"));
    auto* overview = append(pane.root(), "save-overview");
    for (const auto& file : backup.files) {
        const std::string when = save_time(file);
        append_text_element(overview, "heading", save_label(file));
        append_text_element(overview, "small",
                            when.empty() ? format_bytes(file.size)
                                         : fmt::format("Saved {} \xC2\xB7 {}", when, format_bytes(file.size)));
    }
    pane.add_section("Restore");
    append_text_element(pane.root(), "small", "Your current saves are backed up before they are replaced.");
    pane.add_button("Restore This Backup...").on_pressed([backup] { begin_restore(backup); });
    pane.add_button("Open Backups Folder").on_pressed([] { open_folder(backups_dir()); });
    append_text_element(pane.root(), "file-path", borealis::io::fs_path_to_string(backup.path));

    pane.add_section("Danger Zone");
    auto& remove = pane.add_button("Delete This Backup...");
    remove.root()->SetClass("danger", true);
    remove.on_pressed([backup] { begin_delete_backup(backup); });
}

void build_empty_detail(Pane& pane, const Storage& storage, bool haveBackups) {
    append_header(pane.root(), "No save yet", fmt::format("{} \xC2\xB7 Card A", storage_label(storage)));
    auto* overview = append(pane.root(), "save-overview");
    append_text_element(overview, "heading", "Nothing on the memory card");
    append_text_element(overview, "small",
                        haveBackups ? "Start a new game, import a save, or restore one of your backups."
                                    : "Start a new game, or import a save from Dolphin or another copy.");
    pane.add_section("Transfer");
    append_text_element(pane.root(), "small", "Import a .gci file, or the Pikmin 2 save from a Dolphin .raw memory card.");
    pane.add_button(ControlledButton::Props{
                        .text       = "Import Save...",
                        .isDisabled = [] { return borealis::file_select::busy(); },
                    })
        .on_pressed(&begin_import);
    pane.add_button("Open Save Folder").on_pressed([folder = save_folder(storage)] { open_folder(folder); });
    append_text_element(pane.root(), "file-path", borealis::io::fs_path_to_string(storage.path));
}

// Section header of the list with the Import button beside it.
class ListHeader final : public Component {
public:
    ListHeader(Rml::Element* parent, const Rml::String& title, bool withImport)
        : Component(append(parent, "header")) {
        append_text_element(mRoot, "section-heading", title);
        if (withImport) {
            add_child<IconButton>(IconButton::Props{
                                      .icon       = "sim_card_download",
                                      .label      = "Import Save",
                                      .isDisabled = [] { return borealis::file_select::busy(); },
                                  })
                .on_pressed(&begin_import);
        }
    }
};

} // namespace

// ---------------------------------------------------------------------------------------------
// SavesWindow
// ---------------------------------------------------------------------------------------------

SavesWindow::SavesWindow()
    : Window(Props{.tabBar = false, .styleSheets = {"res/rml/saves.rcss"}}) {
    mRoot->SetClass("saves", true);
    set_content([this](Rml::Element* content) { build_content(content); });
}

void SavesWindow::build_content(Rml::Element* content) {
    mGeneration = sGeneration.load();

    auto& list = add_child<Pane>(content, Pane::Type::Controlled);
    list.root()->SetClass("list", true);
    auto& detail = add_child<Pane>(content, Pane::Type::Uncontrolled);
    detail.root()->SetClass("detail", true);

    const Storage storage = active_storage();
    const auto saves      = list_saves(storage);
    const auto backups    = list_backups();

    list.add_child<ListHeader>("Saves", true);
    if (!saves.status) {
        append_text_element(list.root(), "small", saves.status.message);
    } else if (saves.files.empty()) {
        append_text_element(list.root(), "list-empty", "No save on the memory card.");
    }

    // The selection survives rebuilds; when its item is gone, the first save takes over (with no
    // save, the detail pane explains how to get one until a backup is picked).
    const auto exists = [&](const std::string& key) {
        return std::ranges::any_of(saves.files, [&](const SaveFile& f) { return key == "save:" + f.fileName; }) ||
               std::ranges::any_of(backups, [&](const Backup& b) { return key == "backup:" + b.name; });
    };
    if (!exists(mSelected)) {
        mSelected = saves.files.empty() ? std::string{} : "save:" + saves.files.front().fileName;
    }

    const auto add_entry = [&](const std::string& key, const Rml::String& label, std::function<void(Pane&)> build) {
        auto& entry = list.add_group_button({
            .text       = label,
            .isSelected = [this, key] { return mSelected == key; },
        });
        list.register_control(entry, detail, [this, key, build](Pane& pane) {
            if (mSelected != key) {
                mSelected = key;
                play_sound(kSoundItemChange);
            }
            build(pane);
        });
    };

    std::function<void(Pane&)> selectedBuild;
    for (const auto& file : saves.files) {
        const std::string key = "save:" + file.fileName;
        auto build = [storage, file](Pane& pane) { build_save_detail(pane, storage, file); };
        if (key == mSelected) {
            selectedBuild = build;
        }
        add_entry(key, save_label(file), std::move(build));
    }

    list.add_child<ListHeader>("Backups", false);
    if (backups.empty()) {
        append_text_element(list.root(), "list-empty", "No backups yet.");
    }
    for (const auto& backup : backups) {
        const std::string key = "backup:" + backup.name;
        auto build = [backup](Pane& pane) { build_backup_detail(pane, backup); };
        if (key == mSelected) {
            selectedBuild = build;
        }
        add_entry(key, backup_label(backup.name) + (backup.automatic ? "  \xC2\xB7  Auto" : ""), std::move(build));
    }

    if (selectedBuild) {
        selectedBuild(detail);
    } else {
        build_empty_detail(detail, storage, !backups.empty());
    }
}

void SavesWindow::update() {
    if (mGeneration != sGeneration.load()) {
        rebuild_content();
    }
    Window::update();
}

std::string saves_unavailable_reason(bool inGame) {
    if (inGame) {
        return "Return to the start menu to manage saves.";
    }
    if (disc::inspect(settings().backend.discPath.get()).state != disc::State::Ready) {
        return "Choose a Pikmin 2 disc image first.";
    }
    return {};
}

bool open_saves_window() {
    if (!saves_unavailable_reason(false).empty()) {
        return false;
    }
    if (auto* host = top_document()) {
        host->push(std::make_unique<SavesWindow>());
    } else {
        push_document(std::make_unique<SavesWindow>());
    }
    return true;
}

bool saves_restart_pending() {
    return sRestartPending;
}

} // namespace mppm::ui
