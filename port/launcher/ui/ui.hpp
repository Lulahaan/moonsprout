#pragma once

#include <RmlUi/Core.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_power.h>

#include <chrono>
#include <deque>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "nav_types.hpp"
#include "types.hpp"

#include "sound.hpp"

namespace mppm::ui {
class Document;

using clock = std::chrono::steady_clock;

enum class DocumentScope : u8 {
    None,
    CommandConsole,
    Prelaunch,
    Window,
    MenuBar,
    Overlay,
    TouchControls,
    GraphicsTuner,
};

struct Toast {
    Rml::String type;
    Rml::String title;
    Rml::String content;
    clock::duration duration;
    Rml::String modId;
};



struct Insets {
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    float left = 0.0f;

    bool operator==(const Insets& other) const noexcept {
        return top == other.top && right == other.right && bottom == other.bottom &&
               left == other.left;
    }
};

bool initialize() noexcept;
void shutdown() noexcept;

void handle_event(const SDL_Event& event) noexcept;
void update() noexcept;

Document& push_document(
    std::unique_ptr<Document> doc, bool show = true, bool passive = false) noexcept;

namespace detail {
Document& pop_to_or_push_document(bool (*matches)(Document&),
    const std::function<std::unique_ptr<Document>()>& create,
    const std::function<void(Document&)>& configure);
}

template <typename T, typename Configure, typename... Args>
T& pop_to_or_push(Configure&& configure, Args&&... args) {
    return static_cast<T&>(detail::pop_to_or_push_document(
        [](Document& document) { return dynamic_cast<T*>(&document) != nullptr; },
        [&]() -> std::unique_ptr<Document> {
            return std::make_unique<T>(std::forward<Args>(args)...);
        },
        [&](Document& document) {
            std::invoke(std::forward<Configure>(configure), static_cast<T&>(document));
        }));
}

void bring_document_to_front(Document& doc) noexcept;
bool register_scoped_styles(DocumentScope scope, std::string id, const std::string& rcss) noexcept;
void unregister_scoped_styles(DocumentScope scope, std::string_view id) noexcept;
void apply_scoped_styles(Document& doc) noexcept;
void uncover_top_document() noexcept;
Document* find_document(DocumentScope scope) noexcept;
void close_all_documents() noexcept;
bool any_document_visible() noexcept;
// Calls `fn` for every visible document of the focus stack (not passive ones), bottom to top.
void for_each_visible_document(const std::function<void(Document&)>& fn) noexcept;
bool is_prelaunch_open() noexcept;
// The start menu is up and its take-off has started (no status pills over the scene).
bool is_prelaunch_taking_off() noexcept;
bool game_obscured_below(const Document& doc) noexcept;
Document* top_document() noexcept;
// Room kept clear at the top of every large window (context pixels; 0 = none): the in-game bar
// stays up while a window it opened is on screen (DESIGN 8.2), so the window opens below it.
void set_window_top_inset(float px) noexcept;

std::filesystem::path resource_path(const std::filesystem::path& filename) noexcept;
std::string escape(std::string_view str) noexcept;
Rml::Element* append(Rml::Element* parent, const Rml::String& tag) noexcept;
Rml::Element* append_text(Rml::Element* parent, const Rml::String& text) noexcept;
Rml::Element* append_text_element(
    Rml::Element* parent, const Rml::String& tag, const Rml::String& text) noexcept;
void clear_children(Rml::Element* parent) noexcept;
void set_text_content(Rml::Element* parent, const Rml::String& text) noexcept;
void set_display(Rml::Element* element, Rml::Style::Display display) noexcept;

NavCommand map_nav_event(const Rml::Event& event) noexcept;
Insets safe_area_insets(Rml::Context* context) noexcept;

void push_toast(Toast toast) noexcept;
std::deque<Toast>& get_toasts() noexcept;
void show_menu_notification() noexcept;
bool consume_menu_notification_request() noexcept;

const char* battery_icon(SDL_PowerState state, int level) noexcept;
const char* connection_state_icon(SDL_JoystickConnectionState state) noexcept;

// Applies interface.uiScale: a percentage of the window-following base scale (0 = the base).
void apply_scale() noexcept;
// The base UI scale for a window of `width` x `height` framebuffer pixels: a 1280x720 dp frame
// stretched to the window (never below 2/3).
float automatic_ui_scale(float width, float height, float displayScale) noexcept;
// The largest Menu Size (percent) the current window holds; larger values are capped to it.
int max_ui_scale_percent() noexcept;

}  // namespace mppm::ui
