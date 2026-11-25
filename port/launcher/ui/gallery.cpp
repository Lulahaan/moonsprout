#include "gallery.hpp"

#include "bool_button.hpp"
#include "dropdown_button.hpp"
#include "icon_button.hpp"
#include "modal.hpp"
#include "number_button.hpp"
#include "pane.hpp"
#include "row.hpp"
#include "string_button.hpp"
#include "window.hpp"

#include "../settings.hpp"

#include <chrono>
#include <fmt/format.h>

namespace mppm::ui {
namespace {

// Help pane text in the layout the settings window uses: title, body, default, badges.
void show_help(Pane& pane, const char* title, const char* body, const char* fallback = nullptr,
    const char* badge = nullptr) {
    pane.add_rml(fmt::format("<help-title>{}</help-title>", title));
    pane.add_text(body);
    if (fallback != nullptr) {
        pane.add_rml(fmt::format("<help-default>Default: {}</help-default>", fallback));
    }
    if (badge != nullptr) {
        pane.add_rml(fmt::format(
            "<help-badge class=\"restart\"><icon class=\"restart\"/>{}</help-badge>", badge));
    }
}

// A passive sample of the overlay (toasts, FPS pill) and the in-game bar, styled by overlay.rcss
// and popup.rcss, which otherwise only the in-game overlay and menu bar load. It slides in on its
// second frame and removes itself after a few seconds.
class OverlaySample final : public Document {
public:
    explicit OverlaySample(const char* sheet, const char* body)
        : Document(fmt::format(R"RML(<rml><head>
<link type="text/rcss" href="res/rml/theme.rcss" />
<link type="text/rcss" href="res/rml/controls.rcss" />
<link type="text/rcss" href="res/rml/tabbing.rcss" />
<link type="text/rcss" href="res/rml/{}" />
</head><body>{}</body></rml>)RML",
                       sheet, body),
              true, DocumentScope::Overlay),
          mEnd(std::chrono::steady_clock::now() + std::chrono::seconds(6)) {}

    void update() override {
        if (++mFrames == 2) {
            set_open(true);
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= mEnd && !mClosing) {
            mClosing = true;
            set_open(false);
        }
        if (mClosing && now >= mEnd + std::chrono::milliseconds(400)) {
            hide(true);
        }
    }

private:
    void set_open(bool open) {
        Rml::ElementList elements;
        mDocument->QuerySelectorAll(elements, "toast, popup, fps");
        for (auto* element : elements) {
            if (open) {
                element->SetAttribute("open", "");
            } else {
                element->RemoveAttribute("open");
            }
        }
    }

    std::chrono::steady_clock::time_point mEnd;
    int mFrames = 0;
    bool mClosing = false;
};

void show_overlay_samples() {
    push_document(std::make_unique<OverlaySample>("overlay.rcss",
                      R"RML(
<toast class="info"><heading><toast-title>Disc verified</toast-title><icon class="success"/></heading>
  <message><toast-message-text>GPVE01 matches the reference dump.</toast-message-text></message><progress value="0.6"/></toast>
<toast class="warning" style="top: 150dp;"><heading><toast-title>Controller disconnected</toast-title><icon class="warning"/></heading>
  <message><toast-message-text>Reconnect it or pick another device.</toast-message-text></message></toast>
<toast class="menu-notification"><message><row><span>Press</span><b>F1</b><span>(or Back) for the menu</span></row></message></toast>
<fps corner="bl">60 FPS</fps>)RML"),
        true, true);
    push_document(std::make_unique<OverlaySample>("popup.rcss",
                      R"RML(
<popup style="margin-top: 96dp;"><mark class="orb"><leaf class="l"/><leaf class="r"/></mark>
  <tab-bar><tab class="sel">Resume</tab><tab>Settings</tab><tab>Controllers</tab><tab>Start Menu</tab><tab>Quit</tab></tab-bar>
  <divider/><bar-status><span>60 FPS</span><span>21:04</span></bar-status></popup>)RML"),
        true, true);
}

class Gallery final : public Window {
public:
    Gallery() {
        add_tab("Controls", [this](Rml::Element* content) { build_controls(content); });
        add_tab("Buttons", [this](Rml::Element* content) { build_buttons(content); });
        add_tab("Feedback", [this](Rml::Element* content) { build_feedback(content); });
        add_tab("Interface", [this](Rml::Element* content) { build_interface(content); });
        // Labels only, so the bar is as full as the eight-tab settings window.
        for (const char* title : {"Video", "Input", "Audio", "Tools"}) {
            add_tab(title, [this](Rml::Element* content) {
                auto& pane = add_child<Pane>(content, Pane::Type::Uncontrolled);
                pane.add_text("Placeholder tab: it only fills the tab bar.");
            });
        }
    }

private:
    void build_controls(Rml::Element* content) {
        auto& left = add_child<Pane>(content, Pane::Type::Controlled);
        auto& right = add_child<Pane>(content, Pane::Type::Uncontrolled);

        left.add_section("Toggles");
        left.register_control(left.add_child<BoolButton>(BoolButton::Props{
                                  .key = "Pause in background",
                                  .getValue = [this] { return mToggleA; },
                                  .setValue = [this](bool v) { mToggleA = v; },
                              }),
            right, [](Pane& pane) {
                show_help(pane, "Pause in background",
                    "Stops the game while its window is not focused.", "On");
            });
        left.register_control(left.add_child<BoolButton>(BoolButton::Props{
                                  .key = "Show FPS counter",
                                  .getValue = [this] { return mToggleB; },
                                  .setValue = [this](bool v) { mToggleB = v; },
                                  .isModified = [this] { return mToggleB; },
                              }),
            right, [](Pane& pane) {
                show_help(pane, "Show FPS counter",
                    "A modified value is sun yellow, heavier and has a dot in front of it.",
                    "Off");
            });
        left.register_control(left.add_child<BoolButton>(BoolButton::Props{
                                  .key = "Locked by MPPM_VSYNC",
                                  .getValue = [] { return true; },
                                  .setValue = [](bool) {},
                                  .isDisabled = [] { return true; },
                              }),
            right, nullptr);

        left.add_section("Sliders");
        left.register_control(left.add_child<NumberButton>(NumberButton::Props{
                                  .key = "Music volume",
                                  .getValue = [this] { return mVolume; },
                                  .setValue = [this](int v) { mVolume = v; },
                                  .isModified = [this] { return mVolume != 80; },
                                  .min = 0,
                                  .max = 100,
                                  .step = 5,
                                  .suffix = "%",
                              }),
            right, [](Pane& pane) {
                show_help(pane, "Music volume",
                    "Left and Right step by 5. Click or drag the track to set it with the "
                    "mouse; Enter types a value.",
                    "80%");
            });
        left.register_control(left.add_child<NumberButton>(NumberButton::Props{
                                  .key = "Day speed",
                                  .getValue = [this] { return mDaySpeed; },
                                  .setValue = [this](int v) { mDaySpeed = v; },
                                  .min = 25,
                                  .max = 400,
                                  .step = 25,
                                  .suffix = "%",
                              }),
            right, nullptr);
        left.register_control(left.add_child<NumberButton>(NumberButton::Props{
                                  .key = "Unbounded number",
                                  .getValue = [this] { return mUnbounded; },
                                  .setValue = [this](int v) { mUnbounded = v; },
                              }),
            right, [](Pane& pane) {
                show_help(pane, "Unbounded number", "Without a finite range there is no meter.");
            });

        left.add_section("Choices and text");
        static const std::vector<DropdownButton::Option> kAspects{
            {"Automatic"}, {"Original (4:3)"}, {"16:10"}, {"16:9"}, {"21:9"}, {"Stretch", false}};
        left.register_control(left.add_child<DropdownButton>(DropdownButton::Props{
                                  .key = "Aspect ratio",
                                  .options = kAspects,
                                  .getValue = [this] { return mAspect; },
                                  .setValue = [this](int v) { mAspect = v; },
                                  .isModified = [this] { return mAspect != 1; },
                              }),
            right, [](Pane& pane) {
                show_help(pane, "Aspect ratio",
                    "Shape of the picture. Automatic follows the window.", "Original (4:3)",
                    "Needs restart");
            });
        left.register_control(left.add_child<StringButton>(StringButton::Props{
                                  .key = "Player name",
                                  .getValue = [this] { return mName; },
                                  .setValue = [this](Rml::String v) { mName = std::move(v); },
                                  .maxLength = 24,
                              }),
            right, nullptr);
        left.register_control(left.add_group_button({.text = "Controller bindings"}), right,
            [](Pane& pane) { show_help(pane, "Controller bindings", "A group row opens a page."); });
        left.register_control(left.add_select_button({
                                  .key = "Disc",
                                  .getValue = [] { return Rml::String{"GPVE01 (USA)"}; },
                              }),
            right, nullptr);
    }

    void build_buttons(Rml::Element* content) {
        auto& left = add_child<Pane>(content, Pane::Type::Uncontrolled);
        auto& right = add_child<Pane>(content, Pane::Type::Uncontrolled);

        left.add_section("Buttons");
        auto& row1 = left.add_child<Row>(Row::Props{.wrap = true});
        row1.add_child<Button>("Default");
        row1.add_child<Button>("Play").root()->SetClass("primary", true);
        row1.add_child<Button>("Restart now").root()->SetClass("confirm", true);
        row1.add_child<Button>("Delete").root()->SetClass("danger", true);

        auto& row2 = left.add_child<Row>(Row::Props{.wrap = true});
        auto& withIcon = row2.add_child<Button>("");
        withIcon.root()->SetClass("with-icon", true);
        append(withIcon.root(), "icon")->SetClass("folder", true);
        append_text_element(withIcon.root(), "span", "Choose Disc Image");
        row2.add_child<Button>("Compact").root()->SetClass("compact", true);
        row2.add_child<ControlledButton>(ControlledButton::Props{
            .text = "Disabled", .isDisabled = [] { return true; }});

        auto& row3 = left.add_child<Row>(Row::Props{.wrap = true});
        row3.add_child<IconButton>(IconButton::Props{.icon = "settings", .label = "Settings"});
        auto& orbButton =
            row3.add_child<IconButton>(IconButton::Props{.icon = "info", .label = "About"});
        orbButton.root()->SetClass("orb", true);
        auto& compactIcon =
            row3.add_child<IconButton>(IconButton::Props{.icon = "close", .label = "Close"});
        compactIcon.root()->SetClass("compact", true);
        row3.add_child<Button>("With tooltip").set_tooltip("Tooltips repeat the help pane.");

        left.add_section("Dialogs");
        auto& row4 = left.add_child<Row>(Row::Props{.wrap = true});
        row4.add_child<Button>("Open Modal").on_pressed([this] {
            push(std::make_unique<Modal>(Modal::Props{
                .title = "Restart required",
                .bodyText = "Some changes apply after a restart. Restart now?",
                .actions = {{.label = "Later", .onPressed = [](Modal& m) { m.pop(); }},
                    {.label = "Restart Now", .onPressed = [](Modal& m) { m.pop(); }}},
                .icon = "restart",
            }));
        });
        row4.add_child<Button>("Overlay Samples").on_pressed([] { show_overlay_samples(); });
        row4.add_child<Button>("Danger Modal").on_pressed([this] {
            push(std::make_unique<Modal>(Modal::Props{
                .title = "Quit to desktop?",
                .bodyText = "Progress since your last save will be lost.",
                .actions = {{.label = "Cancel", .onPressed = [](Modal& m) { m.pop(); }},
                    {.label = "Quit", .onPressed = [](Modal& m) { m.pop(); }}},
                .variant = "danger",
                .icon = "warning",
            }));
        });

        right.add_rml("<help-title>Buttons</help-title>");
        right.add_text("Pills, one pink primary per screen, green for apply, red text for "
                       "destructive actions. Focus is a sprout ring with a navy outer ring.");
        right.add_rml("<help-badge class=\"restart\"><icon class=\"restart\"/>Needs "
                      "restart</help-badge>");
        right.add_rml("<help-badge class=\"experimental\"><icon class=\"warning\"/>"
                      "Experimental</help-badge>");
        right.add_rml("<help-badge class=\"locked\"><icon class=\"info\"/>Set by "
                      "MPPM_VSYNC</help-badge>");
    }

    void build_feedback(Rml::Element* content) {
        auto& left = add_child<Pane>(content, Pane::Type::Uncontrolled);
        auto& right = add_child<Pane>(content, Pane::Type::Uncontrolled);

        left.add_section("Progress");
        left.add_rml("<progress value=\"0.62\"/>");
        left.add_rml("<progress class=\"success\" value=\"1\"/>");
        left.add_rml("<progress class=\"failed\" value=\"0.35\"/>");
        left.add_rml("<loader><bead/><bead/><bead/></loader>");
        left.add_rml("<div><icon class=\"spinner\" style=\"font-size: 24dp;\"/> Compiling "
                     "shaders</div>");

        left.add_section("Text");
        left.add_text("Body text is Nunito SemiBold. Headings and labels are Fredoka.");
        left.add_rml("<small>Secondary text at 72%, used for captions and help.</small>");
        left.add_rml("<file-path>/home/player/Games/Pikmin 2 (USA).iso</file-path>");
        left.add_rml("<div style=\"display: flex; gap: 16dp;\">"
                     "<status-badge class=\"success\">Verified</status-badge>"
                     "<status-badge class=\"warning\">Unknown dump</status-badge>"
                     "<status-badge class=\"error\">Unreadable</status-badge></div>");

        left.add_section("Prompts and orbs");
        left.add_rml("<button-prompts><prompt><glyph>A</glyph><label>Select</label></prompt>"
                     "<prompt><glyph>B</glyph><label>Back</label></prompt>"
                     "<prompt><glyph>LB</glyph><label>Tab</label></prompt>"
                     "<prompt><glyph class=\"key\">Esc</glyph><label>Close</label></prompt>"
                     "</button-prompts>");
        left.add_rml("<div style=\"display: flex; gap: 16dp; padding: 6dp;\">"
                     "<orb class=\"refract\" style=\"width: 56dp; height: 56dp; position: "
                     "relative;\"><leaf class=\"l\" style=\"left: 16dp; top: 18dp;\"/>"
                     "<leaf class=\"r\" style=\"left: 28dp; top: 18dp;\"/></orb>"
                     "<orb class=\"flat\" style=\"width: 40dp; height: 40dp;\"/></div>");

        right.add_rml("<help-title>Feedback</help-title>");
        right.add_text("Progress is the sun crossing the day meter. Loaders keep pulsing under "
                       "reduce motion because they carry information.");
    }

    void build_interface(Rml::Element* content) {
        auto& left = add_child<Pane>(content, Pane::Type::Controlled);
        auto& right = add_child<Pane>(content, Pane::Type::Uncontrolled);
        auto& ui = settings().interface;

        left.add_section("Live settings");
        left.register_control(left.add_child<NumberButton>(NumberButton::Props{
                                  .key = "Menu size",
                                  .getValue = [&ui] { return ui.uiScale.get(); },
                                  .setValue = [&ui](int v) { ui.uiScale.set(v); },
                                  .isModified = [&ui] { return ui.uiScale.get() != 100; },
                                  .min = 0,
                                  .max = 200,
                                  .step = 5,
                                  .suffix = "%",
                              }),
            right, [](Pane& pane) {
                show_help(pane, "Menu size",
                    "0 is Automatic: about a 1920x1080 dp frame on large windows (DESIGN 9.3).",
                    "100%");
            });
        left.register_control(left.add_child<BoolButton>(BoolButton::Props{
                                  .key = "Reduce motion",
                                  .getValue = [&ui] { return ui.reduceMotion.get(); },
                                  .setValue = [&ui](bool v) { ui.reduceMotion.set(v); },
                              }),
            right, [](Pane& pane) {
                show_help(pane, "Reduce motion",
                    "Nothing slides, grows or bounces; panels fade in place.", "Off");
            });
        left.register_control(left.add_child<BoolButton>(BoolButton::Props{
                                  .key = "High contrast",
                                  .getValue = [&ui] { return ui.highContrast.get(); },
                                  .setValue = [&ui](bool v) { ui.highContrast.set(v); },
                              }),
            right, [](Pane& pane) {
                show_help(pane, "High contrast",
                    "Opaque panels, bright rims and brighter secondary text.", "Off");
            });
        static const std::vector<DropdownButton::Option> kGlass{{"Off"}, {"Blur"}, {"Full"}};
        left.register_control(
            left.add_child<DropdownButton>(DropdownButton::Props{
                .key = "Glass effects",
                .options = kGlass,
                .getValue = [&ui] { return static_cast<int>(ui.glassEffects.get()); },
                .setValue = [&ui](int v) { ui.glassEffects.set(static_cast<GlassEffects>(v)); },
                .isModified = [&ui] { return ui.glassEffects.get() != GlassEffects::Blur; },
            }),
            right, [](Pane& pane) {
                show_help(pane, "Glass effects",
                    "Off draws opaque panels; Full lets small orbs refract the scene.", "Blur");
            });
    }

    bool mToggleA = true;
    bool mToggleB = true;
    int mVolume = 65;
    int mDaySpeed = 100;
    int mUnbounded = 1200;
    int mAspect = 3;
    Rml::String mName = "Olimar";
};

}  // namespace

void open_gallery() noexcept {
    // Not covering what is below keeps the start menu's backdrop (or the game) behind the glass.
    push_document(std::make_unique<Gallery>());
}

}  // namespace mppm::ui
