#include "verify_window.hpp"

#include "settings.hpp"

#include "ui/button.hpp"
#include "ui/format.hpp"
#include "ui/icon_button.hpp"
#include "ui/ui.hpp"
#include "ui/window.hpp"

#include <borealis/io.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <utility>
#include <vector>

namespace mppm::ui {
namespace {

constexpr ByteFormat kByteFormat{.gibFractionDigits = 2, .mibFractionDigits = 0};
// A no-break space: keeps an empty text line at its height, so the dialog doesn't jump.
constexpr const char* kBlankLine = "\xC2\xA0";

// State colours for the header icon and the progress bar; the rest is the shared modal style.
const Rml::String kStyles = R"RCSS(
modal-header icon.verify-state {
    font-size: 32dp;
}
modal-header icon.verify-state.running {
    color: var(--color-info);
}
modal-header icon.verify-state.verified {
    color: var(--color-success);
}
modal-header icon.verify-state.mismatch {
    color: var(--color-warning);
}
modal-header icon.verify-state.failed {
    color: var(--color-error);
}
verification-progress small.eta {
    color: rgba(var(--color-text-rgb), 45%);
}
)RCSS";

bool sOpen = false;

class VerifyDiscWindow final : public WindowSmall {
public:
    VerifyDiscWindow(std::string path, std::function<void(disc::Outcome)> onFinished)
        : WindowSmall("modal")
        , mOnFinished(std::move(onFinished))
        , mVerification(std::make_unique<disc::Verification>(std::move(path)))
        , mStarted(std::chrono::steady_clock::now()) {
        sOpen = true;
        set_document_styles(kStyles);
        auto* header = append(mDialog, "modal-header");
        mTitle       = append(header, "modal-title");
        mIcon        = append(header, "icon");
        mIcon->SetClass("verify-state", true);
        mIcon->SetAttribute("aria-hidden", "true");
        mBody    = append(mDialog, "modal-body");
        mActions = append(mDialog, "modal-actions");
        show_running();
        play_sound(kSoundWindowOpen);
    }

    ~VerifyDiscWindow() override { sOpen = false; }

    void update() override {
        if (mVerification != nullptr) {
            if (mVerification->finished()) {
                finish();
            } else {
                refresh_progress();
            }
        }
        for (auto& button : mButtons) {
            button->update();
        }
        WindowSmall::update();
    }

    bool focus() override {
        // The first action is the safe one ("Cancel", "Done", "Close").
        for (auto& button : mButtons) {
            if (button->focus()) {
                return true;
            }
        }
        return false;
    }

protected:
    bool handle_nav_command(Rml::Event& event, NavCommand cmd) override {
        if (cmd == NavCommand::Cancel || cmd == NavCommand::Menu) {
            if (mVerification != nullptr) {
                request_cancel();
            } else {
                close();
            }
            event.StopPropagation();
            return true;
        }
        if (cmd == NavCommand::Left || cmd == NavCommand::Right) {
            const int direction = cmd == NavCommand::Right ? 1 : -1;
            auto* target        = event.GetTargetElement();
            for (int i = 0; i < static_cast<int>(mButtons.size()); ++i) {
                if (mButtons[i]->contains(target)) {
                    const int next = i + direction;
                    if (next >= 0 && next < static_cast<int>(mButtons.size()) && mButtons[next]->focus()) {
                        play_sound(kSoundItemFocus);
                    }
                    break;
                }
            }
            return true;
        }
        return false;
    }

private:
    void set_header(const Rml::String& title, const char* state, std::string_view glyph) {
        set_text_content(mTitle, title);
        for (const char* name : {"running", "verified", "mismatch", "failed"}) {
            mIcon->SetClass(name, false);
        }
        mIcon->SetClass(state, true);
        set_text_content(mIcon, material_icon(glyph));
    }

    Button& add_action(const Rml::String& label, ButtonCallback onPressed) {
        auto button = std::make_unique<Button>(mActions, label);
        button->root()->SetClass("modal-btn", true);
        button->on_pressed(std::move(onPressed));
        mButtons.push_back(std::move(button));
        return *mButtons.back();
    }

    void clear_actions() {
        mButtons.clear();
        clear_children(mActions);
    }

    void show_running() {
        set_header("Verifying disc", "running", "schedule");
        auto* content = append(mBody, "verification-progress");
        std::string name = borealis::io::display_name(mVerification->path());
        set_text_content(append(content, "file-path"), name.empty() ? mVerification->path() : name);
        mProgress = append(content, "progress");
        mProgress->SetClass("info", true);
        mProgress->SetAttribute("value", 0.0f);
        mDetail = append(content, "small");
        mEta    = append(content, "small");
        mEta->SetClass("eta", true);
        set_text_content(mEta, kBlankLine);
        mCancel = &add_action("Cancel", [this] { request_cancel(); });
        refresh_progress();
    }

    void refresh_progress() {
        if (mVerification == nullptr || mVerification->cancel_requested()) {
            return;
        }
        const uint64_t done  = mVerification->bytes_read();
        const uint64_t total = mVerification->bytes_total();
        if (total == 0) {
            set_text_content(mDetail, "Opening the disc image...");
            set_text_content(mEta, kBlankLine);
            return;
        }
        const float fraction = std::clamp(static_cast<float>(done) / static_cast<float>(total), 0.0f, 1.0f);
        // Only touch the DOM when the shown percentage changes.
        const int percent = static_cast<int>(fraction * 100.0f);
        if (percent == mShownPercent) {
            return;
        }
        mShownPercent = percent;
        mProgress->SetAttribute("value", fraction);
        set_text_content(mDetail, fmt::format("{} of {}  \xC2\xB7  {}%", format_bytes(done, kByteFormat),
                                              format_bytes(total, kByteFormat), percent));
        // A time estimate once the rate has settled (a few percent in, at least two seconds).
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - mStarted).count();
        if (percent >= 3 && elapsed >= 2.0 && done > 0) {
            const double left = elapsed * static_cast<double>(total - done) / static_cast<double>(done);
            set_text_content(mEta, left < 10.0   ? Rml::String("Almost done")
                                   : left < 90.0 ? fmt::format("About {} seconds left", static_cast<int>(left / 5.0 + 0.5) * 5)
                                                 : fmt::format("About {} minutes left", static_cast<int>(left / 60.0 + 0.5)));
        }
    }

    void request_cancel() {
        if (mVerification == nullptr || mVerification->cancel_requested()) {
            return;
        }
        mVerification->cancel();
        if (mCancel != nullptr) {
            mCancel->set_text("Canceling...");
            mCancel->set_disabled(true);
        }
        set_text_content(mDetail, "Stopping...");
        set_text_content(mEta, kBlankLine);
    }

    void finish() {
        const disc::Outcome outcome = mVerification->outcome();
        const std::string path      = mVerification->path();
        mVerification.reset();
        mCancel = nullptr;
        if (outcome == disc::Outcome::Verified) {
            disc::remember_verification(path, DiscVerification::Verified);
        }
        if (mOnFinished) {
            mOnFinished(outcome);
        }
        if (outcome == disc::Outcome::Canceled) {
            close();
            return;
        }

        clear_children(mBody);
        clear_actions();
        auto* text = append(mBody, "p");
        set_text_content(text, disc::outcome_message(outcome));
        switch (outcome) {
        case disc::Outcome::Verified:
            set_header("Disc verified", "verified", "check_circle");
            play_sound(kSoundItemEnable);
            add_action("Done", [this] { close(); });
            break;
        case disc::Outcome::Mismatch:
            set_header("Disc doesn't match", "mismatch", "warning");
            play_sound(kSoundWarning);
            add_action("Close", [this] { close(); });
            add_action("Continue anyway", [this, path] {
                disc::remember_verification(path, DiscVerification::MismatchAccepted);
                close();
            });
            break;
        default:
            set_header("Couldn't verify disc", "failed", "warning");
            play_sound(kSoundWarning);
            add_action("OK", [this] { close(); });
            break;
        }
        focus();
    }

    void close() {
        if (mClosing) {
            return;
        }
        mClosing = true;
        sOpen    = false;
        play_sound(kSoundWindowClose);
        pop();
    }

    std::function<void(disc::Outcome)> mOnFinished;
    std::unique_ptr<disc::Verification> mVerification;
    std::chrono::steady_clock::time_point mStarted;
    Rml::Element* mTitle    = nullptr;
    Rml::Element* mIcon     = nullptr;
    Rml::Element* mBody     = nullptr;
    Rml::Element* mActions  = nullptr;
    Rml::Element* mProgress = nullptr;
    Rml::Element* mDetail   = nullptr;
    Rml::Element* mEta      = nullptr;
    Button* mCancel         = nullptr;
    std::vector<std::unique_ptr<Button>> mButtons;
    int mShownPercent = -1;
    bool mClosing     = false;
};

} // namespace

bool open_verify_disc(std::string path, std::function<void(disc::Outcome)> onFinished) {
    if (sOpen) {
        return false;
    }
    if (path.empty()) {
        path = settings().backend.discPath.get();
    }
    if (path.empty()) {
        return false;
    }
    // A small dialog: like a Modal it sits over the window that opened it, which stays visible
    // (dimmed) underneath and gets its focus back through uncover() when the dialog pops.
    push_document(std::make_unique<VerifyDiscWindow>(std::move(path), std::move(onFinished)));
    if (auto* top = top_document()) {
        top->focus();
    }
    return true;
}

bool disc_verification_running() {
    return sOpen;
}

} // namespace mppm::ui
