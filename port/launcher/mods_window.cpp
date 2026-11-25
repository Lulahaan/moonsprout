#include "mods_window.hpp"

#include "branding.hpp"
#include "prelaunch.hpp"
#include "restart.hpp"

#include "ui/button.hpp"
#include "ui/clamped_text.hpp"
#include "ui/format.hpp"
#include "ui/icon_button.hpp"
#include "ui/input.hpp"
#include "ui/modal.hpp"
#include "ui/pane.hpp"
#include "ui/ui.hpp"

#include <borealis/io.hpp>
#include <fmt/format.h>
#include <p2_mods.h>
#include <p2_mods.hpp>

#include <SDL3/SDL_misc.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <numeric>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace mppm::ui {

// A finished folder scan and the content hash of its mod set, both computed on the worker, and the
// running game's content hash once the game has mounted its mods (the in-game window shows that one;
// the first call reads the executable for the build id, so it never runs on the UI thread).
struct ModsScanResult {
    mods::Scan scan;
    mods::Hash content{};
    bool haveRunning = false;
    mods::Hash running{};
};

namespace {

namespace fs = std::filesystem;
using mods::Hash;
using mods::ModInfo;
using mods::Scan;
using mods::Severity;

// Material Symbols glyphs, as in the settings window's help notes (settings_window.cpp).
constexpr const char* kGlyphLock    = "\xEE\xA2\x97"; // lock
constexpr const char* kGlyphRestart = "\xEF\x81\x93"; // restart_alt
constexpr const char* kGlyphInfo    = "\xEE\xA2\x8E"; // info
constexpr const char* kGlyphWarning = "\xEE\x80\x82"; // warning
constexpr const char* kGlyphError   = "\xEE\x80\x80"; // error

constexpr const char* kDot = "\xC2\xB7";      // middle dot
constexpr const char* kEllipsis = "\xE2\x80\xA6";
constexpr size_t kListNameChars = 34;          // list labels: longer names end in an ellipsis
constexpr size_t kMaxConflictLines = 20;

const char* kBody = "Mods change the game's data files: arenas, caves, tuning and more. They load when the game starts.";
const char* kHashHelp = "Everyone in an online game needs the same content hash. It covers this build of Moonsprout and "
                        "the mods that are on, in their order.";
const char* kOrderHelp = "Mods lower in the list win when two of them change the same file.";
const char* kHashHelpInGame = "The content hash this game was started with: everyone in an online game needs the same one. "
                              "It covers this build of Moonsprout and the mods that loaded, in their order.";
const char* kInGameNote = "Return to the start menu to change mods.";
const char* kChangedInGameNote = "The mods folder has changed since the game started. The list shows the folder; "
                                 "the changes apply after a restart from the start menu.";
const char* kOffNote = "Mods are off for this run (--no-mods).";
const char* kPendingNote = "Changes apply when you press Play: Moonsprout restarts first.";
const char* kReadingNote = "Reading the mods folder\xE2\x80\xA6";

// ---- State shared with the restart flow and between window instances ----

// The mods hash of the folder as the window last read it (mods_restart_pending). It is compared with
// the folder's hash at the boot (Mounted::folderHash), not with the mounted set's: the mount can
// drop a mod for a disc-dependent reason the folder scan cannot see, and that is no change to
// apply, while any change to the files, versions, order or on/off states of the mods is.
bool sHaveLastScan = false;
Hash sLastScanHash{};

// ---- The folder scan, on a worker thread ----
//
// mods::scan() opens, reads and hashes every listed file of every mod (up to 256 MiB per mod), so it
// never runs on the UI thread: one scan at a time runs on a worker, and the newest finished one is
// kept here, so reopening the window shows it at once while the folder is read again.

// The worker is a detached thread that fills a promise: nothing ever joins it, so Quit never waits
// for a scan (a future from std::async would join in its destructor at exit). It only touches what
// it was handed (ScanArgs, copied here), the pure scan and the build id, never the window's state.
using ResultPtr = std::shared_ptr<const ModsScanResult>;
ResultPtr sLatest;            // the newest finished scan; null before the first
std::future<ResultPtr> sJob;  // the scan in flight, if any
bool sJobAfterWrite = false;  // that scan follows a load-order change made in the window

bool scanning() {
    return sJob.valid();
}

ResultPtr scan_now(const mods::ScanArgs& args, bool running) {
    auto result = std::make_shared<ModsScanResult>();
    try {
        result->scan = mods::scan(args);
    } catch (const std::exception&) { // out of memory: report it rather than end the game
        result->scan = Scan{};
        result->scan.issues.push_back({Severity::Error, "", "The mods folder couldn't be read."});
    }
    result->content = mods::content_hash(result->scan);
    if (running) {
        p2_mods_content_hash(result->running.data());
        result->haveRunning = true;
    }
    return result;
}

// Takes the finished scan into sLatest (waiting for it when `wait`) and records what the restart
// flow compares (mods_restart_pending).
void collect(bool wait) {
    if (!sJob.valid() || (!wait && sJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready)) {
        return;
    }
    try {
        sLatest = sJob.get();
    } catch (const std::exception&) { // the worker failed (out of memory): keep the last result
        sJob = {};
        return;
    }
    sHaveLastScan = true;
    sLastScanHash = sLatest->scan.modsHash;
}

void start_scan(bool afterWrite) {
    collect(false);
    if (sJob.valid()) {
        return; // already reading; its result is as new as this one would be
    }
    sJobAfterWrite = afterWrite;
    const mods::ScanArgs args = mods::scan_args();
    const bool running = mods::mounted().ran;
    auto promise = std::make_shared<std::promise<ResultPtr>>();
    sJob = promise->get_future();
    const auto work = [promise, args, running] {
        try {
            promise->set_value(scan_now(args, running));
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
    };
    try {
        std::thread(work).detach();
    } catch (const std::exception&) { // no thread: read it here
        work();
    }
}

bool same_issues(const std::vector<mods::Issue>& a, const std::vector<mods::Issue>& b) {
    return std::ranges::equal(a, b, [](const mods::Issue& x, const mods::Issue& y) {
        return x.severity == y.severity && x.path == y.path && x.message == y.message;
    });
}

// Whether the window would show the same thing for both scans (then a new scan needs no rebuild).
bool same_view(const Scan& a, const Scan& b) {
    if (a.dir != b.dir || a.off != b.off || a.exists != b.exists || a.enabledValid != b.enabledValid ||
        a.modsHash != b.modsHash || a.mods.size() != b.mods.size() || a.conflicts.size() != b.conflicts.size() ||
        !same_issues(a.issues, b.issues)) {
        return false;
    }
    for (size_t i = 0; i < a.mods.size(); ++i) {
        const ModInfo& x = a.mods[i];
        const ModInfo& y = b.mods[i];
        if (x.id != y.id || x.name != y.name || x.version != y.version || x.author != y.author ||
            x.description != y.description || x.listed != y.listed || x.enabled != y.enabled ||
            x.present != y.present || x.valid != y.valid || x.fileCount != y.fileCount ||
            x.totalBytes != y.totalBytes || x.dir != y.dir || !same_issues(x.issues, y.issues)) {
            return false;
        }
    }
    for (size_t i = 0; i < a.conflicts.size(); ++i) {
        const auto& x = a.conflicts[i];
        const auto& y = b.conflicts[i];
        if (x.key != y.key || x.winner != y.winner || x.losers != y.losers) {
            return false;
        }
    }
    return true;
}

// mods_restart_pending without waiting for a scan in flight (for building the window).
bool restart_pending_now();

// The nearest mod above (`up`) or below `index` whose folder exists; scan.mods.size() when none.
// A listed mod whose folder is gone has no place to keep: moving past it would only drop it.
size_t present_neighbour(const Scan& scan, size_t index, bool up) {
    if (up) {
        for (size_t i = index; i-- > 0;) {
            if (scan.mods[i].present) {
                return i;
            }
        }
    } else {
        for (size_t i = index + 1; i < scan.mods.size(); ++i) {
            if (scan.mods[i].present) {
                return i;
            }
        }
    }
    return scan.mods.size();
}

// ---- Text ----

// At most `maxChars` code points, ending in an ellipsis when cut (never inside a UTF-8 sequence).
std::string clip(const std::string& text, size_t maxChars) {
    size_t chars = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
            if (chars == maxChars) {
                std::string out = text.substr(0, i);
                while (!out.empty() && out.back() == ' ') {
                    out.pop_back();
                }
                return out + kEllipsis;
            }
            ++chars;
        }
    }
    return text;
}

const char* state_word(const ModInfo& mod) {
    if (!mod.present) {
        return "Missing";
    }
    if (!mod.valid) {
        return "Problem";
    }
    if (!mod.listed) {
        return "New";
    }
    return mod.enabled ? "On" : "Off";
}

const char* state_line(const ModInfo& mod) {
    if (!mod.present) {
        return "load_order.txt lists this mod, but its folder is gone.";
    }
    if (!mod.valid) {
        return "This mod has problems and won't load.";
    }
    if (!mod.listed) {
        return "New: turn it on to use it.";
    }
    return mod.enabled ? "On" : "Off";
}

std::string display_name(const ModInfo& mod) {
    return mod.name.empty() ? mod.id : mod.name;
}

// Code points in the longest run without a space (a heading in the big font wraps inside a word
// longer than about 14, leaving a letter or two alone on the next line).
size_t longest_word(const std::string& text) {
    size_t best = 0;
    size_t run = 0;
    for (unsigned char c : text) {
        if (c == ' ') {
            run = 0;
        } else if ((c & 0xC0) != 0x80) {
            best = std::max(best, ++run);
        }
    }
    return best;
}

std::string name_of(const Scan& scan, const std::string& id) {
    for (const auto& mod : scan.mods) {
        if (mod.id == id) {
            return display_name(mod);
        }
    }
    return id;
}

Rml::String note_rml(const char* glyph, const char* cls, const Rml::String& text) {
    return fmt::format("<help-note class=\"{}\"><note-icon>{}</note-icon><note-text>{}</note-text></help-note>", cls,
                       glyph, text);
}

Rml::String issue_rml(const mods::Issue& issue) {
    const bool error = issue.severity == Severity::Error;
    Rml::String text = escape(issue.message);
    if (!issue.path.empty()) {
        text += fmt::format("<issue-path>{}</issue-path>", escape(issue.path));
    }
    return note_rml(error ? kGlyphError : kGlyphWarning, error ? "error" : "warning", text);
}

void show_message(Rml::String title, Rml::String body) {
    const auto dismiss = [](Modal& modal) {
        play_sound(kSoundWindowClose);
        modal.pop();
    };
    auto modal = std::make_unique<Modal>(Modal::Props{
        .title     = std::move(title),
        .bodyText  = std::move(body),
        .actions   = {{.label = "OK", .onPressed = dismiss}},
        .onDismiss = dismiss,
        .icon      = "warning",
    });
    if (auto* host = top_document()) {
        host->push(std::move(modal));
    } else {
        push_document(std::move(modal));
    }
}

void open_in_file_manager(const fs::path& folder) {
    const std::string url = "file://" + borealis::io::fs_path_to_generic_string(folder);
    if (!SDL_OpenURL(url.c_str())) {
        show_message("Open folder", "The folder couldn't be opened in the file manager.");
    }
}

void open_mods_folder() {
    std::string error;
    if (!mods::ensure_dir(&error)) {
        play_sound(kSoundWarning);
        show_message("Open Mods Folder", error.empty() ? "The mods folder couldn't be created." : error);
        return;
    }
    const char* dir = p2_mods_dir();
    open_in_file_manager(borealis::io::fs_path_from_utf8(dir != nullptr ? dir : ""));
}

// ---- Components ----

// Text whose content changes while it is on screen (the content hash, once the worker is done).
// Never a focus target.
class LiveText final : public Component {
public:
    LiveText(Rml::Element* parent, const Rml::String& tag, std::function<std::string()> text)
        : Component(append(parent, tag)), mText(std::move(text)) {
        update();
    }
    void update() override {
        std::string text = mText();
        if (text != mCurrent) {
            set_text_content(mRoot, text);
            mCurrent = std::move(text);
        }
        Component::update();
    }
    bool focus() override { return false; }

private:
    std::function<std::string()> mText;
    std::string mCurrent = "\x01";
};

// Like LiveText, as RML (help notes); hidden while empty.
class LiveRml final : public Component {
public:
    LiveRml(Rml::Element* parent, const Rml::String& tag, std::function<Rml::String()> rml)
        : Component(append(parent, tag)), mRml(std::move(rml)) {
        update();
    }
    void update() override {
        Rml::String rml = mRml();
        if (rml != mCurrent) {
            mRoot->SetInnerRML(rml);
            set_display(mRoot, rml.empty() ? Rml::Style::Display::None : Rml::Style::Display::Block);
            mCurrent = std::move(rml);
        }
        Component::update();
    }
    bool focus() override { return false; }

private:
    std::function<Rml::String()> mRml;
    Rml::String mCurrent = "\x01";
};

// A heading clipped to a number of lines (a mod's name can be up to 64 characters).
class ClampedHeading final : public Component {
public:
    ClampedHeading(Rml::Element* parent, const Rml::String& text, int lines) : Component(append(parent, "heading")) {
        add_child<ClampedText>(text, lines);
    }
    bool focus() override { return false; }
};

// Section header of the list with the Open Mods Folder button beside it (the Save Manager's
// ListHeader with its Import button).
class ListHeader final : public Component {
public:
    ListHeader(Rml::Element* parent, const Rml::String& title, bool canOpen) : Component(append(parent, "header")) {
        append_text_element(mRoot, "section-heading", title);
        auto& open = add_child<IconButton>(IconButton::Props{
            .icon       = "folder_open",
            .label      = "Open Mods Folder",
            .isDisabled = [canOpen] { return !canOpen; },
        });
        open.on_pressed(&open_mods_folder);
    }
};

// A load-order button: disabled ones still take the focus (the pane allows it) so the note below
// them can say why, and pressing one gives the warning cue.
ControlledButton& add_order_button(Pane& pane, Rml::String text, bool disabled, std::function<void()> pressed) {
    auto& button = pane.add_button(ControlledButton::Props{
        .text       = std::move(text),
        .isDisabled = [disabled] { return disabled || scanning(); },
    });
    button.on_pressed(std::move(pressed));
    button.set_disabled(disabled || scanning()); // now, so a rebuild can put the focus on it at once
    button.listen(Rml::EventId::Keydown, [&button](Rml::Event& event) {
        if (button.disabled() && map_nav_event(event) == NavCommand::Confirm) {
            play_sound(kSoundWarning);
            event.StopPropagation();
        }
    });
    button.listen(Rml::EventId::Click, [&button](Rml::Event& event) {
        if (button.disabled()) {
            play_sound(kSoundWarning);
            event.StopPropagation();
        }
    });
    return button;
}

bool is_focused(const Component* component) {
    if (component == nullptr) {
        return false;
    }
    auto* context = component->root()->GetContext();
    return context != nullptr && context->GetFocusElement() == component->root();
}

// Button prompts at the bottom of the window (DESIGN 5.17), in the shared <button-prompts> markup
// (global.rcss): the face button of the last used pad, else the key; hidden after a mouse click.
class Prompts final : public Component {
public:
    explicit Prompts(Rml::Element* parent) : Component(append(parent, "button-prompts")) {
        mRoot->SetClass("mods-prompts", true);
        mSelect = add("Enter", "Select");
        mBack   = add("Esc", "Back");
        update();
    }
    void update() override {
        const bool hidden = input::last_input_pointer();
        if (hidden != mRoot->IsClassSet("hidden")) {
            mRoot->SetClass("hidden", hidden);
        }
        const int pad = input::last_input_gamepad() ? 1 : 0;
        if (pad != mPad) {
            mPad = pad;
            for (auto [glyph, key, button] : {std::tuple{mSelect, "Enter", "A"}, std::tuple{mBack, "Esc", "B"}}) {
                glyph->SetClass("key", pad == 0);
                set_text_content(glyph, pad == 1 ? button : key);
            }
        }
    }
    bool focus() override { return false; }

private:
    Rml::Element* add(const char* key, const char* label) {
        auto* prompt = append(mRoot, "prompt");
        auto* glyph = append_text_element(prompt, "glyph", key);
        append_text_element(prompt, "label", label);
        return glyph;
    }
    Rml::Element* mSelect = nullptr;
    Rml::Element* mBack = nullptr;
    int mPad = -1;
};

} // namespace

// ---------------------------------------------------------------------------------------------
// ModsWindow
// ---------------------------------------------------------------------------------------------

ModsWindow::ModsWindow(bool inGame)
    : Window(Props{.tabBar = false, .styleSheets = {"res/rml/mods.rcss"}}), mInGame(inGame), mSelected("overview") {
    mRoot->SetClass("mods", true);
    start_scan(false);
    mResult = sLatest; // the last scan (null the first time), shown until the new one arrives
    set_content([this](Rml::Element* content) { build_content(content); });
    // After the content, so the prompts sit at the bottom edge of the window.
    mPrompts = std::make_unique<Prompts>(mRoot);
}

ModsWindow::~ModsWindow() = default;

const Scan& ModsWindow::scan() const {
    static const Scan kNone;
    return mResult ? mResult->scan : kNone;
}

std::string ModsWindow::hash_text(size_t digits) const {
    if (mInGame && mods::mounted().ran) {
        // In game: the hash of what is loaded (an online game compares this one), not the folder's.
        // The worker computes it (it needs the build id); "…" until then.
        return mResult && mResult->haveRunning ? mods::hex(mResult->running, digits) : std::string(kEllipsis);
    }
    return mResult && !scanning() ? mods::hex(mResult->content, digits) : std::string(kEllipsis);
}

std::string ModsWindow::locked_reason() const {
    if (mInGame) {
        return kInGameNote;
    }
    if (scan().off) {
        return kOffNote;
    }
    return mods::locked_reason();
}

void ModsWindow::build_content(Rml::Element* content) {
    mFocusTarget = nullptr;
    auto& list = add_child<Pane>(content, Pane::Type::Controlled);
    list.root()->SetClass("list", true);
    auto& detail = add_child<Pane>(content, Pane::Type::Uncontrolled);
    detail.root()->SetClass("detail", true);
    detail.set_focus_disabled(true);

    const Scan& scan = this->scan();
    const auto exists = [&](const std::string& key) {
        return key == "overview" ||
               std::ranges::any_of(scan.mods, [&](const ModInfo& m) { return key == "mod:" + m.id; });
    };
    if (!exists(mSelected)) {
        mSelected = "overview";
    }

    // A mod's entry is its name with the state word on a small second line (mods.rcss), so the name
    // has the whole row. Its label is built here, not by the button.
    const auto add_entry = [&](const std::string& key, const Rml::String& label, const char* state,
                               std::function<void(Pane&)> build) {
        auto& entry = list.add_group_button({
            .text       = state == nullptr ? label : Rml::String{},
            .isSelected = [this, key] { return mSelected == key; },
        });
        entry.root()->SetClass("mod-entry", state != nullptr);
        if (state != nullptr) {
            if (auto* keyElem = entry.root()->GetFirstChild(); keyElem != nullptr && keyElem->GetTagName() == "key") {
                append_text_element(keyElem, "mod-name", label);
                append_text_element(keyElem, "mod-state", state);
            }
        }
        list.register_control(entry, detail, [this, key, build](Pane& pane) {
            const bool changed = mSelected != key;
            if (changed) {
                mSelected = key;
                play_sound(kSoundItemChange);
            }
            build(pane);
            if (changed) {
                pane.root()->SetScrollTop(0); // another item: start at its top, not where the last one was
            }
        });
    };

    add_entry("overview", "Overview", nullptr, [this](Pane& pane) { build_overview(pane); });
    list.add_child<ListHeader>("Mods", !scan.off);
    if (!mResult) {
        append_text_element(list.root(), "list-empty", kReadingNote);
    } else if (scan.off) {
        append_text_element(list.root(), "list-empty", "Mods are off for this run.");
    } else if (scan.mods.empty()) {
        append_text_element(list.root(), "list-empty", "No mods yet.");
    }
    for (size_t i = 0; i < scan.mods.size(); ++i) {
        const ModInfo& mod = scan.mods[i];
        add_entry("mod:" + mod.id, clip(display_name(mod), kListNameChars), state_word(mod),
                  [this, i](Pane& pane) { build_mod(pane, i); });
    }

    if (mSelected == "overview") {
        build_overview(detail);
    } else {
        for (size_t i = 0; i < scan.mods.size(); ++i) {
            if (mSelected == "mod:" + scan.mods[i].id) {
                build_mod(detail, i);
            }
        }
    }
}

// Notes at the top of the detail pane (SETTINGS 17): why nothing can change, a pending restart,
// and problems the mount found that the folder scan cannot (the disc-dependent checks).
static Rml::String top_notes(bool inGame, const Scan& scan, const std::string& onlyId) {
    Rml::String rml;
    if (inGame) {
        rml += note_rml(kGlyphInfo, "info", kInGameNote);
        if (restart_pending_now()) {
            rml += note_rml(kGlyphRestart, "restart", kChangedInGameNote);
        }
    } else if (scan.off) {
        rml += note_rml(kGlyphInfo, "info", kOffNote);
    } else if (std::string reason = mods::locked_reason(); !reason.empty()) {
        rml += note_rml(kGlyphLock, "lock", escape(reason));
    }
    if (!inGame && restart_pending_now()) {
        rml += note_rml(kGlyphRestart, "restart pending", kPendingNote);
    }
    const mods::Mounted& mounted = mods::mounted();
    if (mounted.ran) {
        for (const ModInfo& failed : mounted.scan.mods) {
            if (!failed.enabled || failed.valid || !failed.present || (!onlyId.empty() && failed.id != onlyId)) {
                continue;
            }
            // Only what the mount added: the scan's own problems are listed with the mod.
            const ModInfo* now = nullptr;
            for (const ModInfo& m : scan.mods) {
                if (m.id == failed.id) {
                    now = &m;
                }
            }
            for (const mods::Issue& issue : failed.issues) {
                const bool known = now != nullptr && std::ranges::any_of(now->issues, [&](const mods::Issue& i) {
                                       return i.path == issue.path && i.message == issue.message;
                                   });
                if (known || issue.severity != Severity::Error) {
                    continue;
                }
                Rml::String text = fmt::format("{} didn't load: {}", escape(display_name(failed)), escape(issue.message));
                if (!issue.path.empty()) {
                    text += fmt::format("<issue-path>{}</issue-path>", escape(issue.path));
                }
                rml += note_rml(kGlyphError, "error", text);
            }
        }
    }
    return rml;
}

void ModsWindow::build_overview(Pane& pane) {
    const Scan& scan = this->scan();
    if (Rml::String notes = top_notes(mInGame, scan, {}); !notes.empty()) {
        pane.add_rml(notes)->SetClass("mods-notes", true);
    }

    auto* header = append(pane.root(), "mod-header");
    append_text_element(header, "heading", "Mods");
    // In game the count and the hash are those of the running game (mods that loaded).
    const bool running = mInGame && mods::mounted().ran;
    const int on = running ? p2_mods_count() : scan.enabledValid;
    pane.add_child<LiveText>("small", [this, on, running] {
        if (!mResult && !running) {
            return std::string(kReadingNote);
        }
        return on > 0 ? fmt::format("{} on {} content {}", on, kDot, hash_text(8))
                      : fmt::format("Stock content {} {}", kDot, hash_text(8));
    });
    // The subtitle belongs to the header block: move it in (LiveText appends to the pane).
    if (auto* subtitle = pane.root()->GetLastChild()) {
        header->AppendChild(pane.root()->RemoveChild(subtitle));
    }

    append_text_element(pane.root(), "p", kBody)->SetClass("mods-body", true);

    pane.add_section("Content hash");
    pane.add_child<LiveText>("file-path", [this] { return hash_text(64); }).root()->SetClass("mods-hash", true);
    append_text_element(pane.root(), "small", running ? kHashHelpInGame : kHashHelp);

    pane.add_section("Mods folder");
    if (scan.off) {
        append_text_element(pane.root(), "small", kOffNote);
    } else {
        const char* dir = p2_mods_dir();
        append_text_element(pane.root(), "file-path",
                            mResult ? borealis::io::fs_path_to_string(scan.dir) : std::string(dir != nullptr ? dir : ""));
        pane.add_button("Open Mods Folder").on_pressed(&open_mods_folder);
    }
    if (!scan.issues.empty()) {
        Rml::String rml;
        for (const auto& issue : scan.issues) {
            rml += issue_rml(issue);
        }
        pane.add_rml(rml)->SetClass("mods-issues", true);
    }
}

void ModsWindow::build_mod(Pane& pane, size_t index) {
    const Scan& scan = this->scan();
    if (index >= scan.mods.size()) {
        return;
    }
    const ModInfo& mod = scan.mods[index];
    if (Rml::String notes = top_notes(mInGame, scan, mod.id); !notes.empty()) {
        pane.add_rml(notes)->SetClass("mods-notes", true);
    }

    // Header: name / v<version> · <id> · by <author>
    auto* header = append(pane.root(), "mod-header");
    pane.add_child<ClampedHeading>(display_name(mod), 2);
    if (auto* heading = pane.root()->GetLastChild()) {
        heading->SetClass("long-word", longest_word(display_name(mod)) > 14); // smaller font (mods.rcss)
        header->AppendChild(pane.root()->RemoveChild(heading));
    }
    std::string subtitle = mod.version.empty() ? mod.id : fmt::format("v{} {} {}", mod.version, kDot, mod.id);
    if (!mod.author.empty()) {
        subtitle += fmt::format(" {} by {}", kDot, clip(mod.author, 64));
    }
    append_text_element(header, "small", subtitle);

    // Overview block: state, size, description.
    auto* overview = append(pane.root(), "mod-overview");
    const bool on = mod.present && mod.valid && mod.listed && mod.enabled;
    overview->SetClass("on", on);
    overview->SetClass("problem", !mod.present || !mod.valid);
    append_text_element(overview, "heading", state_line(mod));
    if (mod.present) {
        append_text_element(overview, "small",
                            fmt::format("{} file{} {} {}", mod.fileCount, mod.fileCount == 1 ? "" : "s", kDot,
                                        format_bytes(mod.totalBytes)));
    }
    if (!mod.description.empty()) {
        append_text_element(overview, "p", mod.description);
    }

    // Load order.
    const std::string lockReason = locked_reason();
    const bool locked = !lockReason.empty();
    // Places count the mods whose folder exists: a missing one has no place to move to or past.
    const bool hasAbove = present_neighbour(scan, index, true) < scan.mods.size();
    const bool hasBelow = present_neighbour(scan, index, false) < scan.mods.size();
    pane.add_section("Load order");
    append_text_element(pane.root(), "small", kOrderHelp);
    if (mod.present) {
        const auto place = std::count_if(scan.mods.begin(), scan.mods.begin() + static_cast<std::ptrdiff_t>(index),
                                         [](const ModInfo& m) { return m.present; }) + 1;
        const auto places = std::ranges::count_if(scan.mods, [](const ModInfo& m) { return m.present; });
        append_text_element(pane.root(), "small", fmt::format("Position {} of {}.", place, places));
    }

    // A listed mod whose folder is gone can only leave the list (apply drops it on any change).
    const bool canToggle = !locked && (mod.present ? (mod.valid || mod.enabled) : mod.listed);
    const bool canMove = !locked && mod.present && mod.valid;
    const bool canUp = canMove && hasAbove;
    const bool canDown = canMove && hasBelow;
    const bool showsOn = mod.listed && mod.enabled;
    auto& toggle = add_order_button(pane, !mod.present ? "Remove From List" : showsOn ? "Turn Off" : "Turn On", !canToggle,
                                    [this, index] { apply(index, Action::Toggle); });
    auto& up = add_order_button(pane, "Move Up", !canUp, [this, index] { apply(index, Action::Up); });
    auto& down = add_order_button(pane, "Move Down", !canDown, [this, index] { apply(index, Action::Down); });

    // Why the focused button is disabled.
    const auto reason = [=](bool isToggle, bool atEnd, const char* endText) -> Rml::String {
        if (locked) {
            return escape(lockReason);
        }
        if (scanning()) {
            return kReadingNote;
        }
        if (!mod.present) {
            return isToggle ? Rml::String{} : "Its folder is gone. Put it back, or remove it from the list.";
        }
        if (!mod.valid) {
            return isToggle ? "Fix the problems below to turn it on." : "Fix the problems below to move it.";
        }
        return atEnd ? Rml::String(endText) : Rml::String{};
    };
    // Computed when shown: while the folder is being read every button waits for it.
    Component* togglePtr = &toggle;
    Component* upPtr = &up;
    Component* downPtr = &down;
    pane.add_child<LiveRml>("help-notes", [=]() -> Rml::String {
        Rml::String why;
        if (is_focused(togglePtr)) {
            why = canToggle && !scanning() ? Rml::String{} : reason(true, false, "");
        } else if (is_focused(upPtr)) {
            why = canUp && !scanning() ? Rml::String{} : reason(false, !hasAbove, "It is already at the top.");
        } else if (is_focused(downPtr)) {
            why = canDown && !scanning() ? Rml::String{} : reason(false, !hasBelow, "It is already at the bottom.");
        }
        return why.empty() ? Rml::String{} : note_rml(kGlyphInfo, "info", why);
    });
    if (mFocusAction != Action::None) {
        mFocusTarget = mFocusAction == Action::Toggle ? togglePtr : mFocusAction == Action::Up ? upPtr : downPtr;
    }

    // Conflicts this mod is in.
    std::vector<std::string> lines;
    size_t total = 0;
    for (const auto& conflict : scan.conflicts) {
        const bool involved = conflict.winner == mod.id || std::ranges::find(conflict.losers, mod.id) != conflict.losers.end();
        if (!involved) {
            continue;
        }
        const std::string key = conflict.key.starts_with('/') ? conflict.key.substr(1) : conflict.key;
        for (const auto& loser : conflict.losers) {
            if (conflict.winner != mod.id && loser != mod.id) {
                continue;
            }
            ++total;
            if (lines.size() < kMaxConflictLines) {
                // A mod that replaces the whole archive takes part in its inner files' conflicts.
                const char* note = conflict.archiveMod.empty()      ? ""
                                   : conflict.archiveMod == conflict.winner ? " (whole archive)"
                                   : conflict.archiveMod == loser          ? " (on top of the whole archive)"
                                                                           : "";
                lines.push_back(fmt::format("{} overrides {}: {}{}", clip(name_of(scan, conflict.winner), kListNameChars),
                                            clip(name_of(scan, loser), kListNameChars), key, note));
            }
        }
    }
    if (total > 0) {
        pane.add_section("Conflicts");
        auto* block = append(pane.root(), "mod-conflicts");
        for (const auto& line : lines) {
            append_text_element(block, "small", line);
        }
        if (total > lines.size()) {
            append_text_element(block, "small", fmt::format("and {} more", total - lines.size()));
        }
    }

    // Problems (a missing folder has only the state line's own warning: not repeated).
    if (!mod.issues.empty() && mod.present) {
        pane.add_section("Problems");
        Rml::String rml;
        for (const auto& issue : mod.issues) {
            rml += issue_rml(issue);
        }
        pane.add_rml(rml)->SetClass("mods-issues", true);
    }

    // Files.
    pane.add_section("Files");
    if (mod.present) {
        pane.add_button("Open Mod Folder").on_pressed([dir = mod.dir] { open_in_file_manager(dir); });
    }
    append_text_element(pane.root(), "file-path", borealis::io::fs_path_to_string(mod.dir));
}

void ModsWindow::apply(size_t index, Action action) {
    const Scan& scan = this->scan();
    if (index >= scan.mods.size() || !locked_reason().empty() || scanning()) {
        play_sound(kSoundWarning);
        return;
    }
    std::vector<size_t> order(scan.mods.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::vector<bool> enabled;
    for (const auto& mod : scan.mods) {
        enabled.push_back(mod.listed && mod.enabled);
    }
    size_t other = index;
    switch (action) {
    case Action::Toggle:
        enabled[index] = !enabled[index];
        break;
    case Action::Up:
    case Action::Down:
        other = present_neighbour(scan, index, action == Action::Up);
        if (other >= order.size()) {
            play_sound(kSoundWarning);
            return;
        }
        std::swap(order[index], order[other]);
        break;
    case Action::None:
        return;
    }
    // Listed mods keep their place; the acted mod and the one it swapped with join the list (a
    // new mod becomes "Off" once it has a place); a listed mod whose folder is gone is dropped.
    std::vector<mods::OrderEntry> entries;
    for (size_t i : order) {
        const ModInfo& mod = scan.mods[i];
        if (!mod.present || (!mod.listed && i != index && i != other)) {
            continue;
        }
        entries.push_back({.id = mod.id, .enabled = enabled[i]});
    }
    std::string error;
    if (!mods::write_load_order(entries, &error)) {
        play_sound(kSoundWarning);
        show_message("Mods", error.empty() ? "load_order.txt couldn't be written." : error);
        return;
    }
    play_sound(kSoundItemChange);
    start_scan(true);
    mFocusAction = action; // the rebuild comes with the new scan (update)
}

void ModsWindow::update() {
    collect(false);
    if (sLatest && sLatest != mResult && !scanning()) {
        // A new scan: rebuild unless it shows the same (reopening an unchanged folder), and always
        // after a load-order change, which puts the focus back on its button.
        const bool same = mResult && same_view(mResult->scan, sLatest->scan);
        mResult = sLatest;
        mRebuild = mRebuild || !same || mFocusAction != Action::None;
    }
    if (mRebuild) {
        mRebuild = false;
        rebuild_content();
        // The same button of the same mod (Move Up stays on Move Up of the moved mod).
        if (mFocusTarget != nullptr && !mFocusTarget->focus()) {
            mFocusTarget->root()->Focus(true); // disabled: the detail pane lets it take the focus
        }
        if (mFocusTarget == nullptr) {
            focus();
        }
        mFocusAction = Action::None;
        mFocusTarget = nullptr;
    }
    if (mPrompts) {
        mPrompts->update();
    }
    Window::update();
}

void open_mods_window(bool inGame) {
    if (auto* host = top_document()) {
        host->push(std::make_unique<ModsWindow>(inGame));
    } else {
        push_document(std::make_unique<ModsWindow>(inGame));
    }
}

namespace {
bool restart_pending_now() {
    const mods::Mounted& mounted = mods::mounted();
    return launcher::game_booted() && mounted.ran && sHaveLastScan && sLastScanHash != mounted.folderHash;
}
} // namespace

bool mods_restart_pending() {
    if (launcher::game_booted() && mods::mounted().ran) {
        // Waits only for a scan that follows a load-order change made here (Play right after Turn
        // On must see it); a scan from opening the window changes nothing that Play needs, so the
        // last finished one answers.
        collect(sJobAfterWrite);
    }
    return restart_pending_now();
}

} // namespace mppm::ui
