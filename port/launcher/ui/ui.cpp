#include "ui.hpp"

#include "gallery.hpp"
#include "input.hpp"
#include "prelaunch.hpp"
#include "window.hpp"

#include "settings.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/ElementText.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_power.h>
#include <SDL3/SDL_video.h>
#include <unordered_set>
#include <deque>
#include <aurora/lib/window.hpp>
#include <aurora/rmlui.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <ranges>
#include <utility>

namespace mppm::ui {
using mppm::settings;
namespace {

void load_font(const char* filename, bool fallback = false) {
    Rml::LoadFontFace(resource_path(std::filesystem::path("fonts") / filename).generic_string(), fallback);
}

SoundHandler sSoundHandler = nullptr;

bool sInitialized = false;
std::vector<std::unique_ptr<Document>> sDocumentStack;
// Documents that don't participate in the focus stack
std::vector<std::unique_ptr<Document>> sPassiveDocuments;

struct ScopedStyles {
    DocumentScope scope;
    std::string id;
    Rml::SharedPtr<Rml::StyleSheetContainer> sheet;
};

std::vector<ScopedStyles> sScopedStyles;

std::vector<const Rml::StyleSheetContainer*> scoped_sheets(DocumentScope scope) {
    std::vector<const Rml::StyleSheetContainer*> sheets;
    for (const auto& entry : sScopedStyles) {
        if (entry.scope == scope) {
            sheets.push_back(entry.sheet.get());
        }
    }
    return sheets;
}

void restyle_scope(DocumentScope scope) {
    const auto sheets = scoped_sheets(scope);
    const auto restyle_documents = [&sheets, scope](auto& documents) {
        for (auto& doc : documents) {
            if (doc != nullptr && doc->scope() == scope && !doc->closed()) {
                doc->restyle(sheets);
            }
        }
    };
    restyle_documents(sDocumentStack);
    restyle_documents(sPassiveDocuments);
}

std::deque<Toast> sToasts;
bool sMenuNotificationRequested = false;

// Sometimes gamepads can connect and disconnect quickly, especially during
// connection negotiation. In this case, we'll receive an _ADDED event for a
// disconnected gamepad. Storing IDs here lets use only show disconnected
// notifications for gamepads that we sent a connected notification for.
std::unordered_set<SDL_JoystickID> sConnectedGamepads;

// The window size the current automatic scale was computed for.
uint32_t sScaledForWidth = 0;
uint32_t sScaledForHeight = 0;

// MPPM_UI_GALLERY=1 opens the component gallery on the first UI frame (ui/gallery.cpp).
bool sGalleryPending = false;

bool env_enabled(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && std::string_view{value} != "0";
}

}  // namespace

bool initialize() noexcept {
    if (sInitialized) {
        return true;
    }
    if (!aurora::rmlui::is_initialized()) {
        return false;
    }

    // DESIGN.md 3.5: variable fonts register one face per named weight instance, so numeric
    // `font-weight` picks Nunito 200-900, Fredoka 300-700 and Gluten 100-900. Nunito is the
    // fallback face for glyphs the others lack.
    load_font("Nunito-VF.ttf", true);
    load_font("Fredoka-VF.ttf");
    load_font("Gluten-VF.ttf");
    load_font("MaterialSymbolsRounded-Regular.ttf");
    load_font("NotoMono-Regular.ttf");
    // Glass highlights fall from the upper left, like the leaf shine in the wordmark (5.16).
    aurora::rmlui::set_glass_light_dir(-0.5f, -0.8f);
    sGalleryPending = env_enabled("MPPM_UI_GALLERY");
    sInitialized = true;
    return true;
}

void shutdown() noexcept {
    sDocumentStack.clear();
    sPassiveDocuments.clear();
    sConnectedGamepads.clear();
    input::reset_input_state();
    input::release_input_block();
    sInitialized = false;
}

const char* battery_icon(SDL_PowerState state, int level) noexcept {
    if (state == SDL_POWERSTATE_UNKNOWN || state == SDL_POWERSTATE_NO_BATTERY) {
        return "e1a6";  // Battery Unknown
    }
    if (state == SDL_POWERSTATE_ERROR) {
        return "f7ea";  // Battery Error
    }
    if (state == SDL_POWERSTATE_CHARGED || level == 100) {
        return "e1a4";  // Battery Full
    }
    if (state == SDL_POWERSTATE_CHARGING) {
        if (level >= 90) {
            return "f0a7";  // Battery Charging 90
        }
        if (level >= 80) {
            return "f0a6";  // Battery Charging 80
        }
        if (level >= 60) {
            return "f0a5";  // Battery Charging 60
        }
        if (level >= 50) {
            return "f0a4";  // Battery Charging 50
        }
        if (level >= 30) {
            return "f0a3";  // Battery Charging 30
        }
        if (level >= 20) {
            return "f0a2";  // Battery Charging 20
        }
        return "e1a3";  // Battery Charging Full (we use it as empty)
    }
    if (level >= 90) {
        return "ebd2";  // Battery 6 Bar
    }
    if (level >= 80) {
        return "ebd4";  // Battery 5 Bar
    }
    if (level >= 60) {
        return "ebe2";  // Battery 4 Bar
    }
    if (level >= 50) {
        return "ebdd";  // Battery 3 Bar
    }
    if (level >= 30) {
        return "ebe0";  // Battery 2 Bar
    }
    if (level >= 20) {
        return "ebd9";  // Battery 1 Bar
    }
    return "e19c";  // Battery Alert
}

const char* connection_state_icon(SDL_JoystickConnectionState state) noexcept {
    switch (state) {
    case SDL_JOYSTICK_CONNECTION_WIRELESS:
        return "e1a7";
    case SDL_JOYSTICK_CONNECTION_WIRED:
        return "e1e0";
    default:
        return nullptr;
    }
}

void handle_event(const SDL_Event& event) noexcept {
    if (!aurora::rmlui::is_initialized()) {
        return;
    }

    if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
        auto* gamepad = SDL_GetGamepadFromID(event.gdevice.which);
        if (SDL_GamepadConnected(gamepad)) {
            if (settings().interface.controllerToasts.get()) {
                const char* name = SDL_GetGamepadName(gamepad);
                Rml::String content = fmt::format("<span>{}</span>", name ? name : "[Unknown]");
                Rml::String title = "Device Connected";
                if (const char* icon =
                        connection_state_icon(SDL_GetGamepadConnectionState(gamepad)))
                {
                    title = fmt::format(
                        "<row><span>{}</span> <icon class=\"connection\">&#x{};</icon></row>",
                        title, icon);
                }
                int batteryLevel = -1;
                const auto powerState = SDL_GetGamepadPowerInfo(gamepad, &batteryLevel);
                if (powerState != SDL_POWERSTATE_UNKNOWN) {
                    content = fmt::format(
                        "<row>{}</row><row class=\"muted\"><icon class=\"battery\">&#x{};</icon>",
                        content, battery_icon(powerState, batteryLevel));
                    if (batteryLevel > -1) {
                        content = fmt::format("{}&nbsp;<span>{}%</span>", content, batteryLevel);
                    }
                    content += "</row>";
                }
                push_toast({
                    .type = "controller",
                    .title = title,
                    .content = content,
                    .duration = std::chrono::seconds(4),
                });
            }
            sConnectedGamepads.insert(event.gdevice.which);
        }
    } else if (event.type == SDL_EVENT_GAMEPAD_REMOVED &&
               sConnectedGamepads.contains(event.gdevice.which))
    {
        if (settings().interface.controllerToasts.get()) {
            const char* name = SDL_GetGamepadNameForID(event.gdevice.which);
            push_toast({
                .type = "controller",
                .title = "Device Disconnected",
                .content = name ? name : "[Unknown]",
                .duration = std::chrono::seconds(4),
            });
        }
        sConnectedGamepads.erase(event.gdevice.which);
    } else if (event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
        apply_scale();
    }
    input::handle_event(event);
}

bool register_scoped_styles(DocumentScope scope, std::string id, const std::string& rcss) noexcept {
    auto sheet = Rml::Factory::InstanceStyleSheetString(rcss);
    if (sheet == nullptr) {
        return false;
    }
    const auto it = std::ranges::find_if(sScopedStyles,
        [scope, &id](const ScopedStyles& entry) { return entry.scope == scope && entry.id == id; });
    if (it != sScopedStyles.end()) {
        it->sheet = std::move(sheet);
    } else {
        sScopedStyles.push_back({scope, std::move(id), std::move(sheet)});
    }
    restyle_scope(scope);
    return true;
}

void unregister_scoped_styles(DocumentScope scope, std::string_view id) noexcept {
    const auto erased = std::erase_if(sScopedStyles,
        [scope, id](const ScopedStyles& entry) { return entry.scope == scope && entry.id == id; });
    if (erased != 0) {
        restyle_scope(scope);
    }
}

void apply_scoped_styles(Document& doc) noexcept {
    doc.restyle(scoped_sheets(doc.scope()));
}

Document& push_document(std::unique_ptr<Document> doc, bool show, bool passive) noexcept {
    Document& ret = *doc;
    if (passive) {
        sPassiveDocuments.push_back(std::move(doc));
    } else {
        sDocumentStack.push_back(std::move(doc));
    }
    if (show) {
        ret.show();
    }
    input::sync_input_block();
    return ret;
}

Document& detail::pop_to_or_push_document(bool (*matches)(Document&),
    const std::function<std::unique_ptr<Document>()>& create,
    const std::function<void(Document&)>& configure) {
    Document* destination = nullptr;
    size_t destinationIndex = 0;
    for (size_t i = sDocumentStack.size(); i > 0; --i) {
        auto& document = *sDocumentStack[i - 1];
        if (!document.closed() && !document.pending_close() && matches(document)) {
            destination = &document;
            destinationIndex = i - 1;
            break;
        }
    }

    if (destination != nullptr) {
        std::vector<Document*> closing;
        for (size_t i = sDocumentStack.size(); i > destinationIndex + 1; --i) {
            closing.push_back(sDocumentStack[i - 1].get());
        }
        for (auto* document : closing) {
            if (!document->closed() && !document->pending_close()) {
                if (document->visible()) {
                    document->hide(true);
                } else {
                    document->force_hide(true);
                }
            }
        }
        configure(*destination);
    } else {
        auto document = create();
        configure(*document);
        if (auto* current = top_document()) {
            current->cover();
        }
        destination = &push_document(std::move(document), false);
    }

    destination->show();
    destination->focus();
    input::sync_input_block();
    return *destination;
}

void bring_document_to_front(Document& doc) noexcept {
    const auto it = std::ranges::find_if(
        sDocumentStack, [&doc](const auto& entry) { return entry.get() == &doc; });
    if (it == sDocumentStack.end() || std::next(it) == sDocumentStack.end()) {
        return;
    }
    auto entry = std::move(*it);
    sDocumentStack.erase(it);
    sDocumentStack.push_back(std::move(entry));
}

void uncover_top_document() noexcept {
    if (auto* doc = top_document()) {
        doc->uncover();
    }
    input::sync_input_block();
}

Document* find_document(DocumentScope scope) noexcept {
    for (auto& doc : std::views::reverse(sDocumentStack)) {
        if (!doc->closed() && doc->scope() == scope) {
            return doc.get();
        }
    }
    return nullptr;
}

void close_all_documents() noexcept {
    for (auto& doc : sDocumentStack) {
        if (!doc->closed()) {
            doc->force_hide(!doc->permanent());
        }
    }
    input::sync_input_block();
}

bool any_document_visible() noexcept {
    return std::any_of(sDocumentStack.begin(), sDocumentStack.end(),
        [](const auto& doc) { return doc && doc->visible() && !doc->pending_close(); });
}

void for_each_visible_document(const std::function<void(Document&)>& fn) noexcept {
    for (auto& doc : sDocumentStack) {
        if (doc && !doc->closed() && !doc->pending_close() && doc->visible()) {
            fn(*doc);
        }
    }
}

bool is_prelaunch_open() noexcept {
    return std::any_of(sDocumentStack.begin(), sDocumentStack.end(), [](const auto& doc) {
        const auto* prelaunch = dynamic_cast<const Prelaunch*>(doc.get());
        return prelaunch != nullptr && prelaunch->active();
    });
}

bool is_prelaunch_taking_off() noexcept {
    return std::any_of(sDocumentStack.begin(), sDocumentStack.end(), [](const auto& doc) {
        const auto* prelaunch = dynamic_cast<const Prelaunch*>(doc.get());
        return prelaunch != nullptr && prelaunch->taking_off();
    });
}

bool game_obscured_below(const Document& doc) noexcept {
    for (const auto& entry : sDocumentStack) {
        if (entry.get() == &doc) {
            break;
        }
        if (entry->active() && entry->obscures_game()) {
            return true;
        }
    }
    return false;
}

Document* top_document() noexcept {
    for (auto& doc : std::views::reverse(sDocumentStack)) {
        if (doc->active()) {
            return doc.get();
        }
    }
    return nullptr;
}

void update() noexcept {
    if (!aurora::rmlui::is_initialized()) {
        return;
    }

    input::update_input();
    // The scale follows the window, and a resize arrives as an Aurora event the UI does not see:
    // compare the size each frame (two loads and a compare).
    {
        const AuroraWindowSize size = aurora::window::get_window_size();
        if (size.native_fb_width != sScaledForWidth || size.native_fb_height != sScaledForHeight) {
            apply_scale();
        }
    }
    if (std::exchange(sGalleryPending, false)) {
        open_gallery();
    }
    const auto update_documents = [](auto& documents) {
        const size_t count = documents.size();
        for (size_t i = 0; i < count && i < documents.size(); ++i) {
            Document* doc = documents[i].get();
            if (doc != nullptr && !doc->closed()) {
                doc->update();
            }
        }
    };
    update_documents(sDocumentStack);
    update_documents(sPassiveDocuments);

    // Remove closed documents
    {
        const auto [first, last] =
            std::ranges::remove_if(sDocumentStack, [](const auto& doc) { return doc->closed(); });
        sDocumentStack.erase(first, last);
    }
    {
        const auto [first, last] = std::ranges::remove_if(
            sPassiveDocuments, [](const auto& doc) { return doc->closed(); });
        sPassiveDocuments.erase(first, last);
    }

    // Keep focus on the highest active document.
    if (aurora::rmlui::get_context() != nullptr) {
        for (auto& doc : std::views::reverse(sDocumentStack)) {
            if (doc->active() && (doc->has_focus() || doc->focus())) {
                break;
            }
        }
    }

    input::sync_input_block();
}

std::filesystem::path resource_path(const std::filesystem::path& filename) noexcept {
    return std::filesystem::path("res") / filename;
}

std::string escape(std::string_view str) noexcept {
    std::string result;
    result.reserve(str.size());
    for (const char c : str) {
        switch (c) {
        case '&':
            result += "&amp;";
            break;
        case '<':
            result += "&lt;";
            break;
        case '>':
            result += "&gt;";
            break;
        case '"':
            result += "&quot;";
            break;
        default:
            result += c;
            break;
        }
    }
    return result;
}

Rml::Element* append(Rml::Element* parent, const Rml::String& tag) noexcept {
    if (parent == nullptr) {
        return nullptr;
    }
    auto* doc = parent->GetOwnerDocument();
    if (doc == nullptr) {
        return nullptr;
    }
    return parent->AppendChild(doc->CreateElement(tag));
}

Rml::Element* append_text(Rml::Element* parent, const Rml::String& text) noexcept {
    if (parent == nullptr) {
        return nullptr;
    }
    auto* doc = parent->GetOwnerDocument();
    if (doc == nullptr) {
        return nullptr;
    }
    return parent->AppendChild(doc->CreateTextNode(text));
}

Rml::Element* append_text_element(
    Rml::Element* parent, const Rml::String& tag, const Rml::String& text) noexcept {
    auto* element = append(parent, tag);
    append_text(element, text);
    return element;
}

void clear_children(Rml::Element* parent) noexcept {
    if (parent == nullptr) {
        return;
    }
    while (parent->GetNumChildren() > 0) {
        parent->RemoveChild(parent->GetFirstChild());
    }
}

void set_text_content(Rml::Element* parent, const Rml::String& text) noexcept {
    if (parent == nullptr) {
        return;
    }
    if (!text.empty() && parent->GetNumChildren() == 1) {
        if (auto* element = dynamic_cast<Rml::ElementText*>(parent->GetFirstChild())) {
            // RmlUi only dirties layout when the node's text changes.
            element->SetText(text);
            return;
        }
    }
    clear_children(parent);
    if (!text.empty()) {
        append_text(parent, text);
    }
}

void set_display(Rml::Element* element, Rml::Style::Display display) noexcept {
    const Rml::Property value{display};
    const auto* current = element->GetLocalProperty(Rml::PropertyId::Display);
    if (current == nullptr || *current != value) {
        element->SetProperty(Rml::PropertyId::Display, value);
    }
}

NavCommand map_nav_event(const Rml::Event& event) noexcept {
    const auto key = static_cast<Rml::Input::KeyIdentifier>(
        event.GetParameter<int>("key_identifier", Rml::Input::KI_UNKNOWN));
    switch (key) {
    case Rml::Input::KeyIdentifier::KI_UP:
        return NavCommand::Up;
    case Rml::Input::KeyIdentifier::KI_DOWN:
        return NavCommand::Down;
    case Rml::Input::KeyIdentifier::KI_LEFT:
        return NavCommand::Left;
    case Rml::Input::KeyIdentifier::KI_RIGHT:
        return NavCommand::Right;
    case Rml::Input::KeyIdentifier::KI_ESCAPE:
        return NavCommand::Cancel;
    case Rml::Input::KeyIdentifier::KI_RETURN:
    case Rml::Input::KeyIdentifier::KI_NUMPADENTER:
        return NavCommand::Confirm;
    case Rml::Input::KeyIdentifier::KI_F1:
        return event.GetParameter<int>("shift_key", 0) ? NavCommand::None : NavCommand::Menu;
    case Rml::Input::KeyIdentifier::KI_NEXT:
        return NavCommand::Next;
    case Rml::Input::KeyIdentifier::KI_PRIOR:
        return NavCommand::Previous;
    default:
        return NavCommand::None;
    }
}

Insets safe_area_insets(Rml::Context* context) noexcept {
    if (context == nullptr) {
        return {};
    }

    auto* window = aurora::window::get_sdl_window();
    if (window == nullptr) {
        return {};
    }

    const AuroraWindowSize windowSize = aurora::window::get_window_size();
    if (windowSize.width == 0 || windowSize.height == 0) {
        return {};
    }

    SDL_Rect safeRect{};
    if (!SDL_GetWindowSafeArea(window, &safeRect)) {
        return {};
    }

    const Rml::Vector2i contextSize = context->GetDimensions();
    const float scaleX = static_cast<float>(contextSize.x) / static_cast<float>(windowSize.width);
    const float scaleY = static_cast<float>(contextSize.y) / static_cast<float>(windowSize.height);

    const float safeRight = static_cast<float>(safeRect.x + safeRect.w);
    const float safeBottom = static_cast<float>(safeRect.y + safeRect.h);
    return {
        .top = std::max(0.0f, static_cast<float>(safeRect.y)) * scaleY,
        .right = std::max(0.0f, static_cast<float>(windowSize.width) - safeRight) * scaleX,
        .bottom = std::max(0.0f, static_cast<float>(windowSize.height) - safeBottom) * scaleY,
        .left = std::max(0.0f, static_cast<float>(safeRect.x)) * scaleX,
    };
}

void push_toast(Toast toast) noexcept {
    sToasts.push_back(std::move(toast));
}

std::deque<Toast>& get_toasts() noexcept {
    return sToasts;
}

void show_menu_notification() noexcept {
    sMenuNotificationRequested = true;
}

bool consume_menu_notification_request() noexcept {
    const bool requested = sMenuNotificationRequested;
    sMenuNotificationRequested = false;
    return requested;
}

float automatic_ui_scale(float width, float height, float displayScale) noexcept {
    // One rule for every document (start menu, windows, overlay): the menus are laid out for a
    // 1280x720 dp frame and that frame is stretched to the window, so the settings window keeps the
    // same share of a 1920x1080 or 3840x2160 window as of a 720p one. The window is measured in
    // framebuffer pixels, which already include the OS display scale. Nothing goes below 2/3, the
    // scale that keeps a 640x480 window at the 960x720 dp the layout is made for.
    if (width <= 0.0f || height <= 0.0f) {
        return displayScale > 0.0f ? displayScale : 1.0f;
    }
    const float fit = std::min(width / 1280.0f, height / 720.0f);
    return std::max(fit, 2.0f / 3.0f);
}

namespace {

// The largest scale that still leaves the 960x540 dp viewport the layout holds at (DESIGN 9.3).
float max_ui_scale(float width, float height) noexcept {
    if (width <= 0.0f || height <= 0.0f) {
        return 4.0f;
    }
    return std::max(std::min(width / 960.0f, height / 540.0f), 2.0f / 3.0f);
}

}  // namespace

int max_ui_scale_percent() noexcept {
    const AuroraWindowSize size = aurora::window::get_window_size();
    const float w = static_cast<float>(size.native_fb_width);
    const float h = static_cast<float>(size.native_fb_height);
    const float base = automatic_ui_scale(w, h, size.scale);
    return static_cast<int>(std::floor(max_ui_scale(w, h) / base * 100.0f + 0.01f));
}

void apply_scale() noexcept {
    const AuroraWindowSize size = aurora::window::get_window_size();
    const float w = static_cast<float>(size.native_fb_width);
    const float h = static_cast<float>(size.native_fb_height);
    // Menu Size is a percentage of the window-following base (0, "Automatic", is the base itself).
    // Large sizes are capped at what the window holds, so 200% never squeezes the layout below
    // 960x540 dp; small sizes are the user's choice.
    const int userScale = settings().interface.uiScale.get();
    const float percent = userScale > 0 ? static_cast<float>(userScale) / 100.0f : 1.0f;
    const float base = automatic_ui_scale(w, h, size.scale);
    float scale = base * percent;
    if (percent > 1.0f) {
        scale = std::max(base, std::min(scale, max_ui_scale(w, h)));
    }
    sScaledForWidth = size.native_fb_width;
    sScaledForHeight = size.native_fb_height;
    aurora::rmlui::set_ui_scale(scale);
}

void set_sound_handler(SoundHandler handler) noexcept {
    sSoundHandler = handler;
}

void play_sound(UiSound sound) noexcept {
    // Menu Sounds (SETTINGS 5): the one switch for every launcher cue.
    if (sSoundHandler != nullptr && sound != kSoundNone && settings().audio.menuSounds.get()) {
        sSoundHandler(sound);
    }
}

}  // namespace mppm::ui
