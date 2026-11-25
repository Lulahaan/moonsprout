#pragma once
// Controller and keyboard configuration window (docs/launcher/SETTINGS.md 8). Opened from the
// settings window (INPUT > Configure Controllers) and from the in-game bar (Controllers).
//
// One tab per player. Each tab picks the player's controller and whether the keyboard also plays
// for them, then edits that device's bindings with press-to-bind: confirm a row, press the input.
// An input already used by another row of the same player swaps with it. Dead zones, trigger
// thresholds and rumble strength are per controller. Everything is stored by Aurora in its own
// files in the data folder (*.controller, keyboard_bindings.dat, controller_ports.dat), never in
// config.json.
//
// Layout: pages on the left, the selected page in the middle, and on wide windows an info column
// with a controller diagram that lights the GameCube input under the cursor and says what it does
// in Pikmin 2.

#include "ui/window.hpp"

#include <dolphin/pad.h>

#include <array>
#include <optional>
#include <vector>

namespace mppm::ui {

class Pane;
struct BindingSource; // a physical input: key, controller axis direction or controller button

class ControllerConfigWindow : public Window {
public:
    // `port`: the player tab shown first (0 = Player 1).
    explicit ControllerConfigWindow(int port = 0);
    ~ControllerConfigWindow() override;

    void update() override;
    void hide(bool close) override;

    // A GameCube input: a button bit (PAD_BUTTON_*, PAD_TRIGGER_*) or an axis direction (PAD_AXIS_*).
    struct Input {
        bool axis = false;
        u16 id = 0;
        bool operator==(const Input&) const = default;
    };
    // Which of the player's two possible devices a binding belongs to.
    enum class Device { Controller, Keyboard };

protected:
    bool handle_nav_command(Rml::Event& event, NavCommand cmd) override;
    bool consume_close_request() override;

private:
    enum class Page { Device, KeyboardToggle, Keyboard, Buttons, Sticks, Triggers, DeadZones, Rumble, Restore };

    // What the info column shows: a title, a line of help and the diagram parts to light.
    struct Info {
        Rml::String title;
        Rml::String text;
        Rml::String parts;     // space-separated diagram part names, e.g. "a" or "stick cstick"
        Rml::String direction; // stick lean: "up", "down", "left", "right" or empty
        Rml::String note;      // small print under the text
    };

    struct Capture {
        int port = -1;
        Device device = Device::Controller;
        Input input;
        double startedAt = 0.0;
        bool armed = false; // the input that opened the capture has been released
    };

    void build_port_tab(Rml::Element* content, int port);
    void render_page(Pane& pane, int port, Page page);
    void render_device_page(Pane& pane, int port);
    void render_bindings(Pane& pane, int port, Device device, Page page);
    void render_dead_zones(Pane& pane, int port);
    void render_rumble(Pane& pane, int port);
    void add_binding_row(Pane& pane, int port, Device device, Input input);
    void build_info_pane(Rml::Element* content);

    void set_info(Info info);
    void refresh_info();
    void show_notice(const Rml::String& text);
    void clear_notice();

    void begin_capture(int port, Device device, Input input);
    void poll_capture();
    void commit_capture(const BindingSource& source);
    void clear_capture_binding();
    void end_capture(bool changed);
    void cancel_capture();
    bool capturing(int port, Device device, Input input) const;

    void restore_defaults(int port);
    void start_rumble_test(int port);
    void stop_rumble_test();
    void mark_dirty();
    void save_now();
    u64 device_signature(int port) const;
    void rebuild_tab();

    Pane* mLeftPane = nullptr;
    std::vector<Component*> mLeftItems;   // focusable rows of the left pane, in order
    std::vector<Page> mLeftPages;         // the page each of those rows opens
    std::vector<Component*> mMiddleItems; // focusable rows of the middle pane, in order
    Pane* mMiddlePane = nullptr;
    Rml::Element* mInfoRoot = nullptr;
    Rml::Element* mDiagram = nullptr;
    Rml::Element* mInfoTitle = nullptr;
    Rml::Element* mInfoText = nullptr;
    Rml::Element* mInfoHint = nullptr;
    Rml::Element* mNotice = nullptr;
    Info mInfo;
    Page mPage = Page::Device;
    int mActivePort = 0;
    u64 mSignature = 0;

    std::optional<Capture> mCapture;
    bool mSuppressUntilNeutral = false; // swallow navigation until the bound input is released
    int mSuppressPort = -1;
    Rml::String mNoticeText;
    double mNoticeUntil = 0.0;
    int mNoticePort = -1;            // the player tab and page the notice is about
    Page mNoticePage = Page::Device;
    double mRumbleUntil = 0.0;
    int mRumblePort = -1;
    double mSaveAt = 0.0; // pending PADSerializeMappings (debounced while sliders move)
    bool mRefocusRestore = false; // put the focus back on Restore Defaults once its modal is gone
};

// Opens the controller window on `port`'s tab, or brings an open one to the front.
void open_controller_config(int port = 0);

} // namespace mppm::ui
